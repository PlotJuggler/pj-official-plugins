// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chat_session.hpp"

namespace assistant_agent {

class SettingsStore;

// The conversation as it survives the panel instance: written to the host's
// pj.settings.v1 store after every completed turn, read back when a fresh
// instance comes up (closing the toolbox or the app destroys the dialog and
// both backend memories with it).
//
// Deliberately NOT part of the toolbox's layout recipe (saveConfig): the
// settings store is per-user and per-machine, so a shared layout file never
// carries a conversation, and reloading a layout neither resurrects nor
// destroys one. "New chat" erases this state too — the reset must free the
// user from the past, not just hide it until the next reopen.
struct ConversationState {
  // The transcript rows, verbatim (roles + text). Claude's own context comes
  // back through --resume; these exist so the reopened panel SHOWS what the
  // model remembers instead of resuming invisibly.
  std::vector<ChatMessage> messages;
  std::string claude_session_id;    // resumes the CLI session (--resume)
  std::string claude_catalog_hash;  // ClaudeMemory::sent_catalog_hash, verbatim
  [[nodiscard]] bool empty() const {
    return messages.empty() && claude_session_id.empty() && claude_catalog_hash.empty();
  }
};

// FNV-1a (64-bit), hex-encoded. The catalog hash is persisted and compared
// across processes, which rules out std::hash (unspecified and free to differ
// between runs). Inline here so claude_backend links no persistence code for
// the hash alone.
[[nodiscard]] inline std::string fnv1aHex(std::string_view text) {
  std::uint64_t hash = 1469598103934665603ULL;  // FNV offset basis
  for (const char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ULL;  // FNV prime
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (std::size_t i = 16; i-- > 0;) {
    out[i] = kHex[hash & 0xF];
    hash >>= 4;
  }
  return out;
}

// Cap applied at save time so the shared QSettings file cannot grow without
// bound under a long conversation. Oldest rows are dropped WHOLE (never
// truncated mid-message): the transcript keeps its newest rows. The cap budgets
// the message TEXT; the stored JSON adds its per-row envelope on top, so it is
// a bound, not an exact size.
inline constexpr std::size_t kMaxTranscriptBytes = 128 * 1024;

// Absent keys, empty values, or malformed JSON all load as a clean empty
// state — a corrupt store must never take the panel down with it.
[[nodiscard]] ConversationState loadConversation(const SettingsStore& store);
void saveConversation(SettingsStore& store, const ConversationState& state);
// Equivalent to saving an empty state: every key is overwritten with "".
void eraseConversation(SettingsStore& store);
// One-shot cleanup of what the retired Ollama backend left in the store — its
// settings keys and its persisted history. Self-terminating: gated on the
// keys' current values. Run once when the settings view is first bound.
void scrubRetiredOllamaKeys(SettingsStore& store);

}  // namespace assistant_agent
