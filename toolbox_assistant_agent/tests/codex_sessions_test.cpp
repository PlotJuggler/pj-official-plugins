// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Fixtures under tests/fixtures/codex_sessions/2026/09/03/ are built from
// real `codex exec` session files this plugin's own harness store produced
// (~/.codex/sessions/2026/09/03/, on Codex CLI 0.153.0) — trimmed to
// session_meta/response_item/event_msg lines, base_instructions.text
// shortened to one sentence, and the user prompts left as they are (they are
// test prompts already). One fixture keeps a real cwd mismatch (a probe run
// against a different work dir); one has zero assistant messages (a
// cancelled turn); one carries a corrupt line. Every test runs against a
// throwaway copy (see CodexSessionsTest::SetUp) — nothing here ever touches
// a real ~/.codex store.
#include "codex_sessions.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/conversation_helpers.hpp"  // findById
#include "support/scoped_env.hpp"

#ifndef ASSISTANT_CODEX_SESSIONS_FIXTURES_DIR
#error "ASSISTANT_CODEX_SESSIONS_FIXTURES_DIR must be defined by CMake"
#endif

namespace {

using assistant_agent::ChatMessage;
using assistant_agent::codexSessionsDir;
using assistant_agent::ConversationSummary;
using assistant_agent::deleteCodexConversation;
using assistant_agent::listCodexConversations;
using assistant_agent::loadCodexTranscript;

using assistant_agent::testing::findById;
using assistant_agent::testing::makeTempDir;
using assistant_agent::testing::ScopedEnv;

// The cwd every "matching" fixture carries (rollout-*-codex-normal-01.jsonl,
// -codex-noreply-01.jsonl, -codex-corrupt-01.jsonl); -codex-wrongcwd-01.jsonl
// deliberately carries a different one.
constexpr const char* kWorkDir = "/home/alvvm/.local/state/pj-assistant-cli";

// Copies every fixture (recursing its YYYY/MM/DD layout) into a fresh temp
// directory per test, so listCodexConversations/loadCodexTranscript/
// deleteCodexConversation run against a throwaway store.
class CodexSessionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = makeTempDir("assistant_codex_sessions_test_");
    ASSERT_FALSE(dir_.empty());

    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(ASSISTANT_CODEX_SESSIONS_FIXTURES_DIR)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const std::filesystem::path rel = std::filesystem::relative(entry.path(), ASSISTANT_CODEX_SESSIONS_FIXTURES_DIR);
      const std::filesystem::path dest = dir_ / rel;
      std::filesystem::create_directories(dest.parent_path(), ec);
      ASSERT_FALSE(ec) << dest.parent_path() << ": " << ec.message();
      std::filesystem::copy_file(entry.path(), dest, ec);
      ASSERT_FALSE(ec) << entry.path() << ": " << ec.message();
    }
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
};

// --- codexSessionsDir --------------------------------------------------

TEST(CodexSessionsDir, HonorsCodexHomeOverride) {
  ScopedEnv cfg("CODEX_HOME", "/tmp/custom-codex-home");
  EXPECT_EQ(codexSessionsDir(), std::filesystem::path("/tmp/custom-codex-home/sessions"));
}

TEST(CodexSessionsDir, FallsBackToHomeDotCodex) {
  ScopedEnv cfg("CODEX_HOME", nullptr);
  ScopedEnv home("HOME", "/home/testuser");
  EXPECT_EQ(codexSessionsDir(), std::filesystem::path("/home/testuser/.codex/sessions"));
}

TEST(CodexSessionsDir, EmptyWhenNeitherVariableResolves) {
  ScopedEnv cfg("CODEX_HOME", nullptr);
  ScopedEnv home("HOME", nullptr);
  EXPECT_TRUE(codexSessionsDir().empty());
}

TEST(CodexSessionsDir, MissingDirectoryListsAsEmptyNotAnError) {
  EXPECT_TRUE(listCodexConversations("/does/not/exist/at/all", kWorkDir).empty());
}

// --- listCodexConversations: cwd filter, zero-assistant drop, order, title -

TEST_F(CodexSessionsTest, FiltersByCwdDropsZeroAssistantAndSortsNewestFirst) {
  const std::vector<ConversationSummary> convs = listCodexConversations(dir_, kWorkDir);

  // -codex-wrongcwd-01 carries a different cwd (its own real value from the
  // spike) and must not appear even though its last_ts is the newest of all
  // four fixtures.
  EXPECT_EQ(findById(convs, "codex-wrongcwd-01"), nullptr);
  // -codex-noreply-01 has zero assistant messages (a cancelled turn).
  EXPECT_EQ(findById(convs, "codex-noreply-01"), nullptr);

  ASSERT_EQ(convs.size(), 2u);
  // -codex-normal-01's last turn (08:45:40.200) is newer than
  // -codex-corrupt-01's (08:45:11.000) -- newest first.
  EXPECT_EQ(convs[0].id, "codex-normal-01");
  EXPECT_EQ(convs[1].id, "codex-corrupt-01");
}

TEST_F(CodexSessionsTest, TitleIsTheFirstRealUserTextHarnessInjectedOnesSkipped) {
  const std::vector<ConversationSummary> convs = listCodexConversations(dir_, kWorkDir);
  const ConversationSummary* normal = findById(convs, "codex-normal-01");
  ASSERT_NE(normal, nullptr);
  // The fixture's first response_item is a developer record, then a
  // harness-injected "<recommended_plugins>" user record, THEN the real
  // prompt -- the title must skip both and land on the real one.
  EXPECT_EQ(normal->title, "Remember the number 417. Reply only: ok");
  EXPECT_EQ(normal->assistant_messages, 3);
  EXPECT_EQ(normal->first_ts, "2026-09-03T08:44:03.220Z");
  EXPECT_EQ(normal->last_ts, "2026-09-03T08:45:40.200Z");
}

TEST_F(CodexSessionsTest, ACorruptLineInTheMiddleIsTolerated) {
  const std::vector<ConversationSummary> convs = listCodexConversations(dir_, kWorkDir);
  const ConversationSummary* corrupt = findById(convs, "codex-corrupt-01");
  ASSERT_NE(corrupt, nullptr);
  EXPECT_EQ(corrupt->title, "Say hello");
  EXPECT_EQ(corrupt->assistant_messages, 1);
}

// --- loadCodexTranscript: replay order/roles, harness noise skipped --------

TEST_F(CodexSessionsTest, TranscriptSkipsDeveloperAndInjectedContextRows) {
  const std::vector<ChatMessage> rows = loadCodexTranscript(dir_, "codex-normal-01");
  // 3 user + 3 assistant rows; the two developer records and the
  // <recommended_plugins>/<environment_context> injections carry no row.
  ASSERT_EQ(rows.size(), 6u);
  EXPECT_EQ(rows[0].role, ChatMessage::Role::User);
  EXPECT_EQ(rows[0].text, "Remember the number 417. Reply only: ok");
  EXPECT_EQ(rows[1].role, ChatMessage::Role::Assistant);
  EXPECT_EQ(rows[1].text, "ok");
  EXPECT_EQ(rows[2].role, ChatMessage::Role::User);
  EXPECT_EQ(rows[2].text, "What number did I ask you to remember? Reply with the number only.");
  EXPECT_EQ(rows[3].role, ChatMessage::Role::Assistant);
  EXPECT_EQ(rows[3].text, "417");
  EXPECT_EQ(rows[4].role, ChatMessage::Role::User);
  EXPECT_EQ(rows[4].text, "And what was that number again? Number only.");
  EXPECT_EQ(rows[5].role, ChatMessage::Role::Assistant);
  EXPECT_EQ(rows[5].text, "417");
}

TEST_F(CodexSessionsTest, TranscriptToleratesACorruptLine) {
  const std::vector<ChatMessage> rows = loadCodexTranscript(dir_, "codex-corrupt-01");
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].role, ChatMessage::Role::User);
  EXPECT_EQ(rows[0].text, "Say hello");
  EXPECT_EQ(rows[1].role, ChatMessage::Role::Assistant);
  EXPECT_EQ(rows[1].text, "hello");
}

TEST_F(CodexSessionsTest, TranscriptForMissingIdIsEmpty) {
  EXPECT_TRUE(loadCodexTranscript(dir_, "no-such-session").empty());
}

// --- deleteCodexConversation --------------------------------------------

TEST_F(CodexSessionsTest, DeleteRemovesFileAndIgnoresAbsence) {
  const std::filesystem::path file = dir_ / "2026" / "09" / "03" / "rollout-2026-09-03T08-44-03-codex-normal-01.jsonl";
  ASSERT_TRUE(std::filesystem::exists(file));
  EXPECT_TRUE(deleteCodexConversation(dir_, "codex-normal-01"));
  EXPECT_FALSE(std::filesystem::exists(file));

  // Deleting an already-gone conversation is a no-op success.
  EXPECT_TRUE(deleteCodexConversation(dir_, "codex-normal-01"));

  EXPECT_EQ(findById(listCodexConversations(dir_, kWorkDir), "codex-normal-01"), nullptr);
}

}  // namespace
