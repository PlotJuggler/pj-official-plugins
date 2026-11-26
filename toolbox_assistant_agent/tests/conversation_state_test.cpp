// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Covers the active-session-id round trip and the one-shot scrubs of keys older builds left behind
// (conversation_state.hpp).
#include "conversation_state.hpp"

#include <gtest/gtest.h>

#include <pj_base/sdk/settings_store_host.hpp>
#include <string>

#include "settings_store.hpp"

namespace {

using assistant_agent::clearActiveSessionId;
using assistant_agent::ConversationTitles;
using assistant_agent::fnv1aHex;
using assistant_agent::loadActiveSessionId;
using assistant_agent::loadConversationTitles;
using assistant_agent::pruneConversationTitles;
using assistant_agent::removeConversationTitle;
using assistant_agent::saveActiveSessionId;
using assistant_agent::scrubLegacyConversationKeys;
using assistant_agent::scrubRetiredOllamaKeys;
using assistant_agent::setConversationTitle;
using assistant_agent::SettingsStore;

struct Fixture {
  PJ::sdk::InMemorySettingsBackend backend;
  PJ::sdk::SettingsStoreHost host{backend};

  SettingsStore store() {
    return SettingsStore(PJ::sdk::SettingsView{host.view()});
  }
};

TEST(ActiveSessionId, RoundTripsThroughTheStore) {
  Fixture fx;
  auto store = fx.store();
  saveActiveSessionId(store, "sess-123", "claude");
  EXPECT_EQ(loadActiveSessionId(fx.store(), "claude"), "sess-123");
}

TEST(ActiveSessionId, AbsentKeyLoadsAsEmptyString) {
  Fixture fx;
  EXPECT_EQ(loadActiveSessionId(fx.store(), "claude"), "");
}

TEST(ActiveSessionId, ClearLeavesAnEmptyStore) {
  Fixture fx;
  auto store = fx.store();
  saveActiveSessionId(store, "sess-1", "claude");
  clearActiveSessionId(store, "claude");
  EXPECT_EQ(loadActiveSessionId(fx.store(), "claude"), "");
}

TEST(ScrubLegacyConversationKeys, RemovesTheCopiedTranscriptAndCatalogHash) {
  Fixture fx;
  auto store = fx.store();
  // Keys older builds persisted.
  store.setString("assistant.conv.transcript", R"({"v":1,"messages":[{"r":"u","t":"hi"}]})");
  store.setString("assistant.conv.claude.catalog_hash", "deadbeefdeadbeef");

  scrubLegacyConversationKeys(store);
  EXPECT_FALSE(store.contains("assistant.conv.transcript"));
  EXPECT_FALSE(store.contains("assistant.conv.claude.catalog_hash"));

  // Idempotent: a second pass reads two keys and writes nothing.
  scrubLegacyConversationKeys(store);
  EXPECT_FALSE(store.contains("assistant.conv.transcript"));
}

TEST(ScrubLegacyConversationKeys, LeavesTheActiveSessionIdKeyAlone) {
  // The active-id key shares the "assistant.conv.claude.*" prefix with the retired catalog-hash
  // key; the scrub must remove only the latter.
  Fixture fx;
  auto store = fx.store();
  saveActiveSessionId(store, "sess-keep-me", "claude");
  store.setString("assistant.conv.claude.catalog_hash", "stale");

  scrubLegacyConversationKeys(store);
  EXPECT_EQ(loadActiveSessionId(fx.store(), "claude"), "sess-keep-me");
  EXPECT_FALSE(store.contains("assistant.conv.claude.catalog_hash"));
}

TEST(ScrubRetiredOllamaKeys, ErasesEveryRetiredOllamaKey) {
  Fixture fx;
  auto store = fx.store();
  // A retired Ollama backend's settings and up to 256 KB of history.
  store.setString("assistant.ollama.url", "http://box:11434");
  store.setString("assistant.ollama.model", "qwen2.5");
  store.setString("assistant.conv.ollama.history", R"([{"role":"user","content":"old"}])");

  scrubRetiredOllamaKeys(store);
  EXPECT_EQ(store.getString("assistant.ollama.url", "unset"), "");
  EXPECT_EQ(store.getString("assistant.ollama.model", "unset"), "");
  EXPECT_EQ(store.getString("assistant.conv.ollama.history", "unset"), "");

  // Idempotent: a second pass writes nothing new.
  scrubRetiredOllamaKeys(store);
  EXPECT_EQ(store.getString("assistant.conv.ollama.history", "unset"), "");
}

TEST(ConversationTitlesTest, RoundTripsThroughTheStore) {
  Fixture fx;
  auto store = fx.store();
  setConversationTitle(store, "claude", "sess-1", "My demo");
  setConversationTitle(store, "claude", "sess-2", "Another one");

  const ConversationTitles titles = loadConversationTitles(fx.store(), "claude");
  ASSERT_EQ(titles.size(), 2u);
  EXPECT_EQ(titles.at("sess-1"), "My demo");
  EXPECT_EQ(titles.at("sess-2"), "Another one");
}

TEST(ConversationTitlesTest, AbsentKeyLoadsAsAnEmptyMap) {
  Fixture fx;
  EXPECT_TRUE(loadConversationTitles(fx.store(), "claude").empty());
}

TEST(ConversationTitlesTest, RemoveDropsOnlyThatEntry) {
  Fixture fx;
  auto store = fx.store();
  setConversationTitle(store, "claude", "sess-1", "A");
  setConversationTitle(store, "claude", "sess-2", "B");

  removeConversationTitle(store, "claude", "sess-1");

  const ConversationTitles titles = loadConversationTitles(fx.store(), "claude");
  ASSERT_EQ(titles.size(), 1u);
  EXPECT_EQ(titles.at("sess-2"), "B");
}

TEST(ConversationTitlesTest, RemovingAnIdNotInTheMapIsANoOpWrite) {
  Fixture fx;
  auto store = fx.store();
  setConversationTitle(store, "claude", "sess-1", "A");
  removeConversationTitle(store, "claude", "sess-does-not-exist");
  EXPECT_EQ(loadConversationTitles(fx.store(), "claude").at("sess-1"), "A");
}

TEST(ConversationTitlesTest, DifferentBackendKeysDoNotShareNames) {
  Fixture fx;
  auto store = fx.store();
  setConversationTitle(store, "claude", "sess-1", "Claude name");
  setConversationTitle(store, "codex", "sess-1", "Codex name");

  EXPECT_EQ(loadConversationTitles(fx.store(), "claude").at("sess-1"), "Claude name");
  EXPECT_EQ(loadConversationTitles(fx.store(), "codex").at("sess-1"), "Codex name");
}

TEST(ConversationTitlesTest, PruneDropsEntriesNotInTheLiveList) {
  Fixture fx;
  auto store = fx.store();
  setConversationTitle(store, "claude", "sess-1", "Keep me");
  setConversationTitle(store, "claude", "sess-2", "Purge me");

  pruneConversationTitles(store, "claude", {"sess-1"});

  const ConversationTitles titles = loadConversationTitles(fx.store(), "claude");
  ASSERT_EQ(titles.size(), 1u);
  EXPECT_EQ(titles.at("sess-1"), "Keep me");
}

TEST(ConversationTitlesTest, PruneWritesNothingWhenNothingChanged) {
  Fixture fx;
  auto store = fx.store();
  setConversationTitle(store, "claude", "sess-1", "Keep me");

  // sess-2 is not in the map; nothing is dropped.
  pruneConversationTitles(store, "claude", {"sess-1", "sess-2"});

  EXPECT_EQ(loadConversationTitles(fx.store(), "claude").at("sess-1"), "Keep me");
}

TEST(ConversationTitlesTest, PruningEverythingAwayLeavesTheStoreReadingAsEmpty) {
  Fixture fx;
  auto store = fx.store();
  setConversationTitle(store, "claude", "sess-1", "Gone soon");

  pruneConversationTitles(store, "claude", {});  // no live ids

  EXPECT_TRUE(loadConversationTitles(fx.store(), "claude").empty());
}

TEST(ConversationTitlesTest, MalformedJsonDegradesToNoCustomNamesRatherThanThrowing) {
  Fixture fx;
  auto store = fx.store();
  store.setString("assistant.conv.claude.titles", "{not valid json");

  ConversationTitles titles;
  EXPECT_NO_THROW(titles = loadConversationTitles(fx.store(), "claude"));
  EXPECT_TRUE(titles.empty());
}

TEST(ConversationTitlesTest, ANonObjectJsonValueDegradesToNoCustomNames) {
  Fixture fx;
  auto store = fx.store();
  store.setString("assistant.conv.claude.titles", "[1,2,3]");  // valid JSON, wrong shape
  EXPECT_TRUE(loadConversationTitles(fx.store(), "claude").empty());
}

TEST(Fnv1aHex, StableAndDistinct) {
  // Compared across turns (HarnessMemory::sent_catalog_hash dedup), so it must be a fixed function
  // of the input, not of the run.
  EXPECT_EQ(fnv1aHex("catalog"), fnv1aHex("catalog"));
  EXPECT_NE(fnv1aHex("catalog"), fnv1aHex("catalog2"));
  EXPECT_EQ(fnv1aHex(""), fnv1aHex(""));
  EXPECT_EQ(fnv1aHex("abc").size(), 16u);
}

}  // namespace
