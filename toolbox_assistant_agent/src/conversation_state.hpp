// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace assistant_agent {

class SettingsStore;

// What survives closing the toolbox or the app is now just ONE id per backend,
// written to the host's pj.settings.v1 store after a turn establishes or
// changes it: the harness session to resume on the next open. The
// conversation itself — the transcript, its title, the ability to list or
// delete it — is no longer copied into settings; it is read straight from the
// harness's own store (claude_sessions.hpp / codex_sessions.hpp) on demand.
// See docs/ARCHITECTURE.md, "Where the conversation lives".
//
// `key` (default "claude", the only backend that existed before Codex landed)
// selects which backend's id this reads/writes: the persisted key is
// `assistant.conv.<key>.session_id`, so `loadActiveSessionId(store, "claude")`
// reads the exact same string a pre-Codex build wrote — no migration needed.
// AssistantDialog holds one HarnessMemory per key (`memories_`) and calls
// these with the key of whichever backend is currently active, so switching
// backends never silently discards the OTHER one's resume point.
//
// Deliberately NOT part of the toolbox's layout recipe (saveConfig): the
// settings store is per-user and per-machine, so a shared layout file never
// carries a conversation, and reloading a layout neither resurrects nor
// destroys one. "New chat" clears this id too — the reset must free the user
// from the past, not just hide it until the next reopen.
//
// Absent key, unbound store, or a host backend fault all load as "" — a panel
// with nothing to resume, not an error.
[[nodiscard]] std::string loadActiveSessionId(const SettingsStore& store, const std::string& key);
void saveActiveSessionId(SettingsStore& store, const std::string& session_id, const std::string& key);
void clearActiveSessionId(SettingsStore& store, const std::string& key);

// FNV-1a (64-bit), hex-encoded. Used by claude_backend.cpp to dedupe the
// catalog listing sent into a live conversation (HarnessMemory::sent_catalog_hash)
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

// The one thing the drawer copies into settings on purpose: a per-conversation
// display name the user picked from the rename context-menu action, keyed by
// conversation id. One JSON object per backend (`assistant.conv.<key>.titles`
// = {"<id>": "<name>", ...}) rather than a key per conversation: pj.settings.v1
// has get/set/contains/remove but no way to ENUMERATE keys, so a key-per-id
// scheme could never find (and prune) the names of conversations the harness
// has since purged on its own retention -- see pruneConversationTitles.
using ConversationTitles = std::map<std::string, std::string>;

// Malformed JSON (a hand-edited store, or a value this build doesn't
// recognize) degrades to "no custom names" rather than throwing -- a title is
// cosmetic, never worth taking the panel down over.
[[nodiscard]] ConversationTitles loadConversationTitles(const SettingsStore& store, const std::string& key);
void saveConversationTitles(SettingsStore& store, const std::string& key, const ConversationTitles& titles);

// load + mutate + save round trips, for the two call sites that touch exactly
// one entry (the rename commit) instead of the whole map.
void setConversationTitle(
    SettingsStore& store, const std::string& key, const std::string& conversation_id, const std::string& name);
void removeConversationTitle(SettingsStore& store, const std::string& key, const std::string& conversation_id);

// Drops every entry whose id is not in `live_ids`, saving only if something
// actually changed. Called right after the drawer's listing is refreshed, so a
// rename never outlives the conversation it named.
void pruneConversationTitles(SettingsStore& store, const std::string& key, const std::vector<std::string>& live_ids);

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
