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

// The only conversation state persisted across closing the toolbox is ONE harness
// session id per backend (pj.settings.v1, key `assistant.conv.<key>.session_id`,
// `key` = backend name). The transcript lives in the harness's own store and is read
// on demand (claude_sessions.hpp / codex_sessions.hpp; docs/ARCHITECTURE.md, "Where
// the conversation lives"). One id per backend so switching backends keeps the other
// one's resume point.
//
// Deliberately NOT part of the layout recipe (saveConfig): the settings store is
// per-user and per-machine, so a shared layout never carries a conversation. "New
// chat" clears this id too.
//
// Absent key, unbound store, or a host backend fault all load as "": nothing to
// resume, not an error.
[[nodiscard]] std::string loadActiveSessionId(const SettingsStore& store, const std::string& key);
void saveActiveSessionId(SettingsStore& store, const std::string& session_id, const std::string& key);
void clearActiveSessionId(SettingsStore& store, const std::string& key);

// FNV-1a (64-bit), hex-encoded; dedupes the catalog listing sent into a live
// conversation (HarnessMemory::sent_catalog_hash). Not std::hash, which is
// unspecified and may differ between runs.
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

// User-chosen conversation names, keyed by conversation id. One JSON object per
// backend (`assistant.conv.<key>.titles` = {"<id>": "<name>", ...}) rather than a key
// per conversation: pj.settings.v1 cannot enumerate keys, so per-id keys could never
// be pruned once the harness purges a conversation (see pruneConversationTitles).
using ConversationTitles = std::map<std::string, std::string>;

// Malformed JSON degrades to "no custom names" rather than throwing; a title is
// cosmetic.
[[nodiscard]] ConversationTitles loadConversationTitles(const SettingsStore& store, const std::string& key);
void saveConversationTitles(SettingsStore& store, const std::string& key, const ConversationTitles& titles);

// load + mutate + save round trips for a single entry.
void setConversationTitle(
    SettingsStore& store, const std::string& key, const std::string& conversation_id, const std::string& name);
void removeConversationTitle(SettingsStore& store, const std::string& key, const std::string& conversation_id);

// Drops every entry whose id is not in `live_ids`, saving only on change.
void pruneConversationTitles(SettingsStore& store, const std::string& key, const std::vector<std::string>& live_ids);

// Erases the transcript and catalog-hash keys that older builds persisted. Gated on
// SettingsStore::contains: a clean store costs two reads and no writes. Run once when
// the settings view is first bound.
void scrubLegacyConversationKeys(SettingsStore& store);

// Empties the settings keys and history that the retired Ollama backend persisted.
// Gated on the keys' current values. Run once when the settings view is first bound.
void scrubRetiredOllamaKeys(SettingsStore& store);

}  // namespace assistant_agent
