// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "conversation_state.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "settings_store.hpp"

namespace assistant_agent {

namespace {

// `key` is the backend ("claude"/"codex").
std::string activeSessionIdKey(const std::string& key) {
  return "assistant.conv." + key + ".session_id";
}

// Keys older builds wrote and this build no longer does; see scrubLegacyConversationKeys.
constexpr const char* kKeyLegacyTranscript = "assistant.conv.transcript";
constexpr const char* kKeyLegacyCatalogHash = "assistant.conv.claude.catalog_hash";

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
    return {};
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
    // Empty string, not remove(), like clearActiveSessionId.
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
    return;
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
  // Up to 256 KB of dead history; gated on the current values so a clean store
  // costs three reads and no writes.
  for (const char* key : {"assistant.ollama.url", "assistant.ollama.model", "assistant.conv.ollama.history"}) {
    if (!store.getString(key, "").empty()) {
      store.setString(key, "");
    }
  }
}

}  // namespace assistant_agent
