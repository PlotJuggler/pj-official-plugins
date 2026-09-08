// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "conversation_state.hpp"  // fnv1aHex
#include "session_text.hpp"        // kCatalogChangedNote / kResumedConversationNote

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
  // Set when this memory was just populated from a conversation resumed off
  // disk (switchToConversation in assistant_dialog.cpp). The loaded layout
  // and current owned-tab set may differ from the state in that transcript,
  // so the next turn forces a fresh catalog + a note telling the model to
  // inspect what exists now.
  // Consumed (cleared) by composePayload the first time it actually sends
  // that catalog.
  bool resumed_pending = false;
};

// The user text as actually sent: the catalog listing is prepended only when
// its hash differs from what this conversation was already told (first turn,
// or the loaded data changed — the latter carries an explicit note), OR when
// `memory.resumed_pending` forces a resend regardless of the hash (a
// conversation just resumed off disk, whose note says so instead). Updates
// `memory.sent_catalog_hash` and clears `resumed_pending`. Free function,
// backend-agnostic (every harness backend composes its payload the same way),
// so the catalog-note rules are testable without spawning any CLI.
[[nodiscard]] inline std::string composePayload(
    const std::string& text, const std::string& catalog, HarnessMemory& memory) {
  // Prepend the catalog listing to the user's message, but only when it is new
  // to this conversation. --resume replays the whole history, so a listing sent
  // once stays visible on every later turn; sending it again would just pay for
  // the same text twice. A listing that has CHANGED does get re-sent — that is
  // how the model learns the loaded data is not what it was told earlier. A
  // conversation just resumed off disk (resumed_pending) forces a resend too,
  // even if the hash happens to match: the loaded layout and owned tabs may
  // differ from the state described earlier, and the model needs telling.
  if (catalog.empty()) {
    return text;  // nothing to send even when resuming; resumed_pending stays
                  // set for the next turn that actually has a catalog
  }
  const std::string hash = fnv1aHex(catalog);
  const bool resuming = memory.resumed_pending;
  if (hash == memory.sent_catalog_hash && !resuming) {
    return text;
  }
  std::string note;
  if (resuming) {
    note = std::string(kResumedConversationNote) + "\n";
  } else if (!memory.sent_catalog_hash.empty()) {
    note = std::string(kCatalogChangedNote) + "\n";
  }
  memory.sent_catalog_hash = hash;
  memory.resumed_pending = false;
  return catalog + "\n" + note + "\n" + text;
}

}  // namespace assistant_agent
