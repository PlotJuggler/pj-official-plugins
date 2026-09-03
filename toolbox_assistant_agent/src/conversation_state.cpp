// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "conversation_state.hpp"

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
