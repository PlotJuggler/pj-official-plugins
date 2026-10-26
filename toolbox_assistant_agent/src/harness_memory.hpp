// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "conversation_state.hpp"  // fnv1aHex
#include "session_text.hpp"        // the notes composePayload attaches to a listing

namespace assistant_agent {

// What has to survive one backend object for its conversation to continue.
//
// Deliberately NOT owned by any one backend: a backend is a transport (a CLI
// path and a model name) and gets rebuilt whenever either changes, while the
// conversation belongs to the user and does not. Kept together in one struct
// because both fields answer the same question — what this conversation has
// already been told. Shared by every harness backend (ClaudeBackend,
// CodexBackend, ...); the panel holds one instance per backend key so
// switching backends never silently restarts a chat (assistant_dialog.cpp's
// `memories_`).
struct HarnessMemory {
  std::string session_id;  // the harness's own session/thread id, for --resume continuity
  // fnv1aHex of the catalog listing already sent into this conversation.
  // --resume carries the whole history forward, so re-sending an identical
  // listing every turn would be pure waste; re-sending a CHANGED one is how
  // the model finds out the user loaded something else. Purely an in-process
  // dedup key now — the conversation itself lives in the harness's own store
  // (claude_sessions.hpp / codex_sessions.hpp), so this struct is no longer
  // persisted across panel restarts.
  std::string sent_catalog_hash;
  // The same dedup key for the "PlotJuggler features" listing
  // (features_digest.hpp), kept apart from the catalog's: the two change
  // independently (loading a file vs. installing or enabling a plugin), and
  // one changing must not re-send the other.
  std::string sent_features_hash;
  // Set when this memory was just populated from a conversation resumed off
  // disk (switchToConversation in assistant_dialog.cpp). The loaded layout
  // and current owned-tab set may differ from the state in that transcript,
  // so the next turn forces both listings to be re-sent, the catalog with a
  // note telling the model to inspect what exists now.
  // Consumed (cleared) by composePayload once a turn has carried every
  // non-empty listing, and restored with the hashes when that turn fails
  // before the model answers (restoreSentListings).
  bool resumed_pending = false;
};

// What composePayload records as already told, saved before a turn so that a
// turn the model never received can be undone (restoreSentListings). Without
// it a failed first turn (the CLI not logged in, say) would leave both hashes
// set and the retry would go out without either listing.
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

// The user text as actually sent: `features\n\n[note\n\n]catalog\n\n[note\n\n]text`,
// where each listing is prepended only when its own hash differs from what
// this conversation was already told (first turn, or that listing changed —
// a change carries an explicit note), OR when `memory.resumed_pending` forces
// a resend regardless of the hashes (a conversation just resumed off disk,
// whose catalog note says so instead). An empty listing is never sent. The
// two hashes are independent; `resumed_pending` is cleared once every
// non-empty listing has gone out in one call. Free function, backend-agnostic
// (every harness backend composes its payload the same way), so these rules
// are testable without spawning any CLI.
[[nodiscard]] inline std::string composePayload(
    const std::string& text, const std::string& features, const std::string& catalog, HarnessMemory& memory) {
  // --resume replays the whole history, so a listing sent once stays visible
  // on every later turn; sending it again would just pay for the same text
  // twice. A listing that has CHANGED does get re-sent — that is how the
  // model learns the loaded data (or the plugin set) is not what it was told
  // earlier. A conversation just resumed off disk (resumed_pending) forces a
  // resend too, even if a hash happens to match: the loaded layout and owned
  // tabs may differ from the state described earlier, and the model needs
  // telling.
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
  // While resuming every non-empty listing is sent, so "something went out"
  // is "everything did". Nothing to send keeps the flag for the next turn
  // that has a listing.
  if (sent_any) {
    memory.resumed_pending = false;
  }
  return prefix + text;
}

}  // namespace assistant_agent
