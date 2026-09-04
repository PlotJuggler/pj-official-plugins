// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "conversation_state.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "settings_store.hpp"

namespace assistant_agent {

namespace {

// Persisted key, next to the assistant.* settings keys (assistant_dialog.cpp)
// in the same shared store. `key` is the backend ("claude"/"codex"); the
// "claude" form is unchanged from the pre-Codex, pre-harness-store design: it
// already meant "the session to resume", just alongside two keys this build
// no longer writes (see scrubLegacyConversationKeys).
std::string activeSessionIdKey(const std::string& key) {
  return "assistant.conv." + key + ".session_id";
}

// What an older build of this plugin persisted and this one does not: the
// full transcript, copied into settings on every completed turn, and the
// catalog hash that rode along with it. Both are superseded by reading the
// harness's own session store on demand (claude_sessions.hpp).
constexpr const char* kKeyLegacyTranscript = "assistant.conv.transcript";
constexpr const char* kKeyLegacyCatalogHash = "assistant.conv.claude.catalog_hash";

// Persisted key for the custom-name map, next to activeSessionIdKey above.
std::string conversationTitlesKey(const std::string& key) {
  return "assistant.conv." + key + ".titles";
}

}  // namespace

std::string loadActiveSessionId(const SettingsStore& store, const std::string& key) {
  return store.getString(activeSessionIdKey(key), "");
}

void saveActiveSessionId(SettingsStore& store, const std::string& session_id, const std::string& key) {
  store.setString(activeSessionIdKey(key), session_id);
}

void clearActiveSessionId(SettingsStore& store, const std::string& key) {
  store.setString(activeSessionIdKey(key), "");
}

ConversationTitles loadConversationTitles(const SettingsStore& store, const std::string& key) {
  const std::string raw = store.getString(conversationTitlesKey(key), "");
  if (raw.empty()) {
    return {};
  }
  const nlohmann::json doc = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object()) {
    return {};  // corrupt store, or a shape this build doesn't recognize -- no names beats throwing
  }
  ConversationTitles titles;
  for (const auto& [id, name] : doc.items()) {
    if (name.is_string()) {
      titles.emplace(id, name.get<std::string>());
    }
  }
  return titles;
}

void saveConversationTitles(SettingsStore& store, const std::string& key, const ConversationTitles& titles) {
  if (titles.empty()) {
    // Mirrors clearActiveSessionId's convention: an empty string, not remove().
    store.setString(conversationTitlesKey(key), "");
    return;
  }
  nlohmann::json doc = nlohmann::json::object();
  for (const auto& [id, name] : titles) {
    doc[id] = name;
  }
  store.setString(conversationTitlesKey(key), doc.dump());
}

void setConversationTitle(
    SettingsStore& store, const std::string& key, const std::string& conversation_id, const std::string& name) {
  ConversationTitles titles = loadConversationTitles(store, key);
  titles[conversation_id] = name;
  saveConversationTitles(store, key, titles);
}

void removeConversationTitle(SettingsStore& store, const std::string& key, const std::string& conversation_id) {
  ConversationTitles titles = loadConversationTitles(store, key);
  if (titles.erase(conversation_id) > 0) {
    saveConversationTitles(store, key, titles);
  }
}

void pruneConversationTitles(SettingsStore& store, const std::string& key, const std::vector<std::string>& live_ids) {
  ConversationTitles titles = loadConversationTitles(store, key);
  if (titles.empty()) {
    return;  // nothing to prune, nothing to write
  }
  bool changed = false;
  for (auto it = titles.begin(); it != titles.end();) {
    if (std::find(live_ids.begin(), live_ids.end(), it->first) == live_ids.end()) {
      it = titles.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) {
    saveConversationTitles(store, key, titles);
  }
}

void scrubLegacyConversationKeys(SettingsStore& store) {
  for (const char* key : {kKeyLegacyTranscript, kKeyLegacyCatalogHash}) {
    if (store.contains(key)) {
      store.remove(key);
    }
  }
}

void scrubRetiredOllamaKeys(SettingsStore& store) {
  // What the retired Ollama backend left behind: its settings and up to 256 KB
  // of dead conversation. Gated on the current values, so a scrubbed store —
  // or one that never saw Ollama — costs three reads and writes nothing.
  for (const char* key : {"assistant.ollama.url", "assistant.ollama.model", "assistant.conv.ollama.history"}) {
    if (!store.getString(key, "").empty()) {
      store.setString(key, "");
    }
  }
}

}  // namespace assistant_agent
