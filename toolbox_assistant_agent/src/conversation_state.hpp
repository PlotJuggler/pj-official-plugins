// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace assistant_agent {

class SettingsStore;

// What survives closing the toolbox or the app is now just ONE id, written to
// the host's pj.settings.v1 store after a turn establishes or changes it: the
// Claude session to resume on the next open. The conversation itself — the
// transcript, its title, the ability to list or delete it — is no longer
// copied into settings; it is read straight from the harness's own store
// (claude_sessions.hpp) on demand. See docs/ARCHITECTURE.md, "Where the
// conversation lives".
//
// Deliberately NOT part of the toolbox's layout recipe (saveConfig): the
// settings store is per-user and per-machine, so a shared layout file never
// carries a conversation, and reloading a layout neither resurrects nor
// destroys one. "New chat" clears this id too — the reset must free the user
// from the past, not just hide it until the next reopen.
//
// Absent key, unbound store, or a host backend fault all load as "" — a panel
// with nothing to resume, not an error.
[[nodiscard]] std::string loadActiveSessionId(const SettingsStore& store);
void saveActiveSessionId(SettingsStore& store, const std::string& session_id);
void clearActiveSessionId(SettingsStore& store);

// FNV-1a (64-bit), hex-encoded. Used by claude_backend.cpp to dedupe the
// catalog listing sent into a live conversation (ClaudeMemory::sent_catalog_hash)
// — an in-process comparison only now, but kept here (rather than moved into
// claude_backend.hpp) as the one small, generically useful pure hash both a
// backend and a future one could share. Not std::hash: that is unspecified
// and free to differ between runs, and a hash meant to be compared has to be a
// fixed function of its input.
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

// One-shot migration off the pre-harness-store design: erases what an older
// build of this plugin left behind — the copied transcript and the persisted
// catalog hash, neither of which this build writes anymore. Gated on
// SettingsStore::contains, so a store already scrubbed (or one that never
// held these keys) costs two reads and writes nothing. Run once when the
// settings view is first bound, next to scrubRetiredOllamaKeys.
void scrubLegacyConversationKeys(SettingsStore& store);

// One-shot cleanup of what the retired Ollama backend left in the store — its
// settings keys and its persisted history. Self-terminating: gated on the
// keys' current values. Run once when the settings view is first bound.
void scrubRetiredOllamaKeys(SettingsStore& store);

}  // namespace assistant_agent
