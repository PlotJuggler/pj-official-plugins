// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The persisted conversation: what survives closing the toolbox or the app.
// Driven through the same in-memory pj.settings.v1 wiring a host would use, so
// "a later panel instance reads it back" is literally a second load from the
// same backend.
#include "conversation_state.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <pj_base/sdk/settings_store_host.hpp>
#include <string>

#include "settings_store.hpp"

namespace {

using assistant_agent::ChatMessage;
using assistant_agent::ConversationState;
using assistant_agent::eraseConversation;
using assistant_agent::fnv1aHex;
using assistant_agent::loadConversation;
using assistant_agent::saveConversation;

struct Fixture {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};

  assistant_agent::SettingsStore store() {
    return assistant_agent::SettingsStore(PJ::sdk::SettingsView{host.view()});
  }
};

TEST(ConversationState, RoundTripsThroughTheStore) {
  Fixture fx;
  ConversationState conv;
  conv.messages = {
      {ChatMessage::Role::User, "how many topics?"},
      {ChatMessage::Role::Tool, "list_topics"},
      {ChatMessage::Role::Assistant, "6 topics are loaded."},
      {ChatMessage::Role::System, "note"},
  };
  conv.claude_session_id = "sess-123";
  conv.claude_catalog_hash = fnv1aHex("catalog v1");
  conv.ollama_history_json = R"([{"role":"system","content":"s"},{"role":"user","content":"u"}])";

  auto store = fx.store();
  saveConversation(store, conv);
  const ConversationState back = loadConversation(fx.store());

  ASSERT_EQ(back.messages.size(), conv.messages.size());
  for (std::size_t i = 0; i < conv.messages.size(); ++i) {
    EXPECT_EQ(back.messages[i].role, conv.messages[i].role) << i;
    EXPECT_EQ(back.messages[i].text, conv.messages[i].text) << i;
  }
  EXPECT_EQ(back.claude_session_id, "sess-123");
  EXPECT_EQ(back.claude_catalog_hash, conv.claude_catalog_hash);
  EXPECT_EQ(back.ollama_history_json, conv.ollama_history_json);
}

TEST(ConversationState, AbsentKeysLoadAsCleanEmptyState) {
  Fixture fx;
  const ConversationState back = loadConversation(fx.store());
  EXPECT_TRUE(back.empty());
}

TEST(ConversationState, MalformedValuesLoadAsCleanEmptyState) {
  Fixture fx;
  auto store = fx.store();
  store.setString("assistant.conv.transcript", "{not json");
  store.setString("assistant.conv.ollama.history", "\"a string, not an array\"");

  const ConversationState back = loadConversation(fx.store());
  EXPECT_TRUE(back.messages.empty());
  EXPECT_TRUE(back.ollama_history_json.empty());
}

TEST(ConversationState, UnknownRowsAreSkippedNotFatal) {
  Fixture fx;
  auto store = fx.store();
  // A future build's row shape ("x") between two this build understands.
  store.setString(
      "assistant.conv.transcript",
      R"({"v":1,"messages":[{"r":"u","t":"one"},{"r":"x","t":"future"},{"r":"a","t":"two"}]})");

  const ConversationState back = loadConversation(fx.store());
  ASSERT_EQ(back.messages.size(), 2u);
  EXPECT_EQ(back.messages[0].text, "one");
  EXPECT_EQ(back.messages[1].text, "two");
}

TEST(ConversationState, TranscriptCapDropsOldestWholeMessages) {
  Fixture fx;
  ConversationState conv;
  // Three messages of ~60 KB each: the 128 KB budget fits the newest two only.
  const std::string big(60 * 1024, 'x');
  conv.messages = {
      {ChatMessage::Role::User, "oldest " + big},
      {ChatMessage::Role::Assistant, "middle " + big},
      {ChatMessage::Role::User, "newest " + big},
  };
  auto store = fx.store();
  saveConversation(store, conv);

  const ConversationState back = loadConversation(fx.store());
  ASSERT_EQ(back.messages.size(), 2u);
  EXPECT_EQ(back.messages[0].text.rfind("middle", 0), 0u);
  EXPECT_EQ(back.messages[1].text.rfind("newest", 0), 0u);
}

TEST(ConversationState, OllamaHistoryCapKeepsSlotZeroAndNewestTurns) {
  Fixture fx;
  ConversationState conv;
  nlohmann::json history = nlohmann::json::array();
  history.push_back({{"role", "system"}, {"content", "the system message"}});
  for (int i = 0; i < 4; ++i) {
    history.push_back({{"role", "user"}, {"content", "turn " + std::to_string(i) + " " + std::string(90 * 1024, 'y')}});
  }
  conv.ollama_history_json = history.dump();  // ~360 KB, over the 256 KB cap
  auto store = fx.store();
  saveConversation(store, conv);

  const ConversationState back = loadConversation(fx.store());
  const nlohmann::json trimmed = nlohmann::json::parse(back.ollama_history_json);
  ASSERT_TRUE(trimmed.is_array());
  ASSERT_GE(trimmed.size(), 2u);
  EXPECT_EQ(trimmed[0]["role"], "system");  // slot 0 survives
  // The newest turn survives; the oldest were dropped.
  EXPECT_EQ(std::string(trimmed[trimmed.size() - 1]["content"]).rfind("turn 3", 0), 0u);
  EXPECT_LE(back.ollama_history_json.size(), assistant_agent::kMaxOllamaHistoryBytes);
}

TEST(ConversationState, EraseLeavesACleanStore) {
  Fixture fx;
  ConversationState conv;
  conv.messages = {{ChatMessage::Role::User, "hello"}};
  conv.claude_session_id = "sess-1";
  auto store = fx.store();
  saveConversation(store, conv);

  eraseConversation(store);
  EXPECT_TRUE(loadConversation(fx.store()).empty());
}

TEST(Fnv1aHex, StableAndDistinct) {
  // Persisted and compared across processes: the value must be a fixed
  // function of the input, not of the run.
  EXPECT_EQ(fnv1aHex("catalog"), fnv1aHex("catalog"));
  EXPECT_NE(fnv1aHex("catalog"), fnv1aHex("catalog2"));
  EXPECT_EQ(fnv1aHex(""), fnv1aHex(""));
  EXPECT_EQ(fnv1aHex("abc").size(), 16u);
}

}  // namespace
