// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "conversation_state.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>

#include "settings_store.hpp"

namespace assistant_agent {

namespace {

using nlohmann::json;

// Persisted keys, next to the assistant.* settings keys (assistant_dialog.cpp)
// in the same shared store.
constexpr const char* kKeyTranscript = "assistant.conv.transcript";
constexpr const char* kKeyClaudeSessionId = "assistant.conv.claude.session_id";
constexpr const char* kKeyClaudeCatalogHash = "assistant.conv.claude.catalog_hash";

// One letter per role: the persisted form should stay boring and versioned
// rather than leak the C++ enum's numeric values into the store.
const char* roleLetter(ChatMessage::Role role) {
  switch (role) {
    case ChatMessage::Role::User:
      return "u";
    case ChatMessage::Role::Assistant:
      return "a";
    case ChatMessage::Role::System:
      return "s";
    case ChatMessage::Role::Tool:
      return "t";
  }
  return "s";
}

std::optional<ChatMessage::Role> roleFromLetter(std::string_view letter) {
  if (letter == "u") {
    return ChatMessage::Role::User;
  }
  if (letter == "a") {
    return ChatMessage::Role::Assistant;
  }
  if (letter == "s") {
    return ChatMessage::Role::System;
  }
  if (letter == "t") {
    return ChatMessage::Role::Tool;
  }
  return std::nullopt;
}

}  // namespace

ConversationState loadConversation(const SettingsStore& store) {
  ConversationState state;
  state.claude_session_id = store.getString(kKeyClaudeSessionId, "");
  state.claude_catalog_hash = store.getString(kKeyClaudeCatalogHash, "");

  const std::string transcript = store.getString(kKeyTranscript, "");
  if (!transcript.empty()) {
    const json doc = json::parse(transcript, nullptr, /*allow_exceptions=*/false);
    // find() on anything that is not an object (a malformed value included)
    // yields end(), so this one lookup is also the shape check.
    const auto messages = doc.find("messages");
    if (messages != doc.end() && messages->is_array()) {
      for (const json& row : *messages) {
        const auto letter = row.find("r");
        const auto text = row.find("t");
        if (letter == row.end() || !letter->is_string() || text == row.end() || !text->is_string()) {
          continue;  // skip what this build does not understand; keep the rest
        }
        const std::optional<ChatMessage::Role> role = roleFromLetter(letter->get_ref<const std::string&>());
        if (!role.has_value()) {
          continue;
        }
        state.messages.push_back({*role, text->get<std::string>()});
      }
    }
  }
  return state;
}

void saveConversation(SettingsStore& store, const ConversationState& state) {
  // Transcript cap: keep the newest suffix whose text fits the budget, dropping
  // the oldest rows whole.
  std::size_t first = state.messages.size();
  std::size_t bytes = 0;
  while (first > 0) {
    const std::size_t row_bytes = state.messages[first - 1].text.size();
    if (bytes + row_bytes > kMaxTranscriptBytes) {
      break;
    }
    bytes += row_bytes;
    --first;
  }
  json rows = json::array();
  for (std::size_t i = first; i < state.messages.size(); ++i) {
    rows.push_back({{"r", roleLetter(state.messages[i].role)}, {"t", state.messages[i].text}});
  }
  store.setString(kKeyTranscript, rows.empty() ? "" : json{{"v", 1}, {"messages", std::move(rows)}}.dump());

  store.setString(kKeyClaudeSessionId, state.claude_session_id);
  store.setString(kKeyClaudeCatalogHash, state.claude_catalog_hash);
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

void eraseConversation(SettingsStore& store) {
  saveConversation(store, ConversationState{});
}

}  // namespace assistant_agent
