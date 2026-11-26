// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "conversation_state.hpp"  // fnv1aHex
#include "session_text.hpp"        // listing notes

namespace assistant_agent {

// What has to survive one backend object for its conversation to continue: what the
// conversation has already been told. Not owned by a backend, which is a transport
// rebuilt whenever its CLI path or model changes. The panel holds one per backend key
// (`memories_` in assistant_dialog.cpp) so switching backends does not restart a chat.
// In-process only; the conversation itself lives in the harness's store.
struct HarnessMemory {
  std::string session_id;  // the harness's own session/thread id, for --resume
  // fnv1aHex of the catalog listing already sent. --resume carries the history, so an
  // identical listing is not re-sent; a CHANGED one is, which is how the model learns
  // the user loaded something else.
  std::string sent_catalog_hash;
  // Same dedup key for the "PlotJuggler features" listing, kept apart because the two
  // change independently (loading a file vs. enabling a plugin).
  std::string sent_features_hash;
  // Set when this memory was populated from a conversation resumed off disk
  // (switchToConversation). The layout and owned tabs may differ from that transcript,
  // so the next turn re-sends both listings, the catalog with a note telling the model
  // to inspect what exists now. Cleared by composePayload once a turn carried every
  // non-empty listing; restored with the hashes when that turn fails before the model
  // answers (restoreSentListings).
  bool resumed_pending = false;
};

// What composePayload records as already told, saved before a turn so a turn the model
// never received can be undone (restoreSentListings); otherwise a failed first turn
// (CLI not logged in) would leave both hashes set and the retry would omit the listings.
struct SentListings {
  std::string features_hash;
  std::string catalog_hash;
  bool resumed_pending = false;
};

[[nodiscard]] inline SentListings sentListings(const HarnessMemory& memory) {
  return {memory.sent_features_hash, memory.sent_catalog_hash, memory.resumed_pending};
}

inline void restoreSentListings(HarnessMemory& memory, SentListings saved) {
  memory.sent_features_hash = std::move(saved.features_hash);
  memory.sent_catalog_hash = std::move(saved.catalog_hash);
  memory.resumed_pending = saved.resumed_pending;
}

// Rollback policy for one turn's listings. Construct before composePayload marks the
// listings as told; call answered() on every event showing the model read the payload
// (text or a tool call); call finish() at the end. A turn that failed before the model
// answered may never have reached it, so finish() forgets its listings were sent. A
// successful turn is never rolled back, even a silent one.
class ListingsTurn {
 public:
  explicit ListingsTurn(HarnessMemory& memory) : memory_(memory), told_before_(sentListings(memory)) {}

  void answered() {
    model_answered_ = true;
  }

  void finish(bool failed) {
    if (failed && !model_answered_) {
      restoreSentListings(memory_, std::move(told_before_));
    }
  }

 private:
  HarnessMemory& memory_;
  SentListings told_before_;
  bool model_answered_ = false;
};

// The user text as sent: `features\n\n[note\n\n]catalog\n\n[note\n\n]text`. Each
// listing is prepended only when its hash differs from what the conversation was told
// (first turn, or changed, with a note), or when `memory.resumed_pending` forces a
// resend regardless of the hashes. An empty listing is never sent; the two hashes are
// independent. Backend-agnostic, so testable without spawning a CLI.
[[nodiscard]] inline std::string composePayload(
    const std::string& text, const std::string& features, const std::string& catalog, HarnessMemory& memory) {
  // resumed_pending forces a resend even when a hash matches: the layout and owned tabs
  // may differ from what the resumed transcript describes.
  const bool resuming = memory.resumed_pending;
  std::string prefix;
  bool sent_any = false;
  const auto prepend = [&](const std::string& listing, std::string& sent_hash, std::string_view changed_note,
                           std::string_view resumed_note) {
    if (listing.empty()) {
      return;
    }
    const std::string hash = fnv1aHex(listing);
    if (hash == sent_hash && !resuming) {
      return;
    }
    std::string_view note;
    if (resuming && !resumed_note.empty()) {
      note = resumed_note;
    } else if (!sent_hash.empty() && hash != sent_hash) {
      note = changed_note;
    }
    sent_hash = hash;
    prefix += listing;
    prefix += "\n\n";
    if (!note.empty()) {
      prefix += note;
      prefix += "\n\n";
    }
    sent_any = true;
  };
  prepend(features, memory.sent_features_hash, kFeaturesChangedNote, {});
  prepend(catalog, memory.sent_catalog_hash, kCatalogChangedNote, kResumedConversationNote);
  // While resuming every non-empty listing is sent, so "something went out" means
  // "everything did". With nothing to send the flag stays for the next turn.
  if (sent_any) {
    memory.resumed_pending = false;
  }
  return prefix + text;
}

}  // namespace assistant_agent
