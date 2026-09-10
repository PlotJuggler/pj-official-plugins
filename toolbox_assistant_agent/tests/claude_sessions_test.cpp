// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Fixtures under tests/fixtures/sessions/ are cut from real Claude Code
// session files this plugin's own harness store produced (paths and the
// user's own name anonymized), plus a few hand-built ones for shapes that do
// not occur naturally (a corrupt line, an unknown future record type, the
// resume note) or that would otherwise pull in a much larger real payload
// just to exercise one field. Every test runs against a throwaway copy (see
// ClaudeSessionsTest::SetUp) — nothing here ever touches the checked-in
// files, so DeleteRemovesFileAndIgnoresAbsence is safe to run repeatedly.
#include "claude_sessions.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/conversation_helpers.hpp"  // findById
#include "support/scoped_env.hpp"

#ifndef ASSISTANT_SESSIONS_FIXTURES_DIR
#error "ASSISTANT_SESSIONS_FIXTURES_DIR must be defined by CMake"
#endif

namespace {

using assistant_agent::ChatMessage;
using assistant_agent::claudeCwdSlug;
using assistant_agent::claudeSessionsDir;
using assistant_agent::ConversationSummary;
using assistant_agent::deleteConversation;
using assistant_agent::formatShortDate;
using assistant_agent::listConversations;
using assistant_agent::loadTranscript;
using assistant_agent::parseIso8601Utc;
using assistant_agent::stripCatalogPrefix;

using assistant_agent::testing::findById;
using assistant_agent::testing::makeTempDir;
using assistant_agent::testing::ScopedEnv;

// Copies every fixture into a fresh temp directory per test, so
// listConversations/loadTranscript/deleteConversation run against a
// throwaway store rather than the checked-in fixtures.
class ClaudeSessionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = makeTempDir("assistant_sessions_test_");
    ASSERT_FALSE(dir_.empty());

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(ASSISTANT_SESSIONS_FIXTURES_DIR)) {
      if (entry.is_regular_file()) {
        std::filesystem::copy_file(entry.path(), dir_ / entry.path().filename(), ec);
        ASSERT_FALSE(ec) << entry.path() << ": " << ec.message();
      }
    }
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
};

// --- (f) the cwd slug and the directory it resolves to --------------------

TEST(ClaudeCwdSlug, ReplacesSlashesAndDots) {
  // Verified against a real store this session: the plugin's fixed cwd
  // "/home/alvvm/.local/state/pj-assistant-cli" maps to exactly this
  // directory name under ~/.claude/projects/.
  EXPECT_EQ(claudeCwdSlug("/home/alvvm/.local/state/pj-assistant-cli"), "-home-alvvm--local-state-pj-assistant-cli");
}

TEST(ClaudeSessionsDir, HonorsClaudeConfigDirOverride) {
  ScopedEnv cfg("CLAUDE_CONFIG_DIR", "/tmp/custom-claude-cfg");
  EXPECT_EQ(
      claudeSessionsDir("/home/user/.local/state/pj-assistant-cli"),
      std::filesystem::path("/tmp/custom-claude-cfg/projects/-home-user--local-state-pj-assistant-cli"));
}

TEST(ClaudeSessionsDir, FallsBackToHomeDotClaude) {
  ScopedEnv cfg("CLAUDE_CONFIG_DIR", nullptr);
  ScopedEnv home("HOME", "/home/testuser");
  EXPECT_EQ(
      claudeSessionsDir("/home/testuser/.local/state/pj-assistant-cli"),
      std::filesystem::path("/home/testuser/.claude/projects/-home-testuser--local-state-pj-assistant-cli"));
}

TEST(ClaudeSessionsDir, EmptyWhenNeitherVariableResolves) {
  ScopedEnv cfg("CLAUDE_CONFIG_DIR", nullptr);
  ScopedEnv home("HOME", nullptr);
  EXPECT_TRUE(claudeSessionsDir("/whatever").empty());
}

TEST(ClaudeSessionsDir, MissingDirectoryListsAsEmptyNotAnError) {
  EXPECT_TRUE(listConversations("/does/not/exist/at/all").empty());
}

// --- (c) stripCatalogPrefix, as a pure function ----------------------------

TEST(StripCatalogPrefix, LeavesOrdinaryTextAlone) {
  EXPECT_EQ(stripCatalogPrefix("derivative of /a/b"), "derivative of /a/b");
}

TEST(StripCatalogPrefix, ShortCatalogNoNote) {
  EXPECT_EQ(stripCatalogPrefix("Loaded data: nothing is loaded yet.\n\nhello?"), "hello?");
}

TEST(StripCatalogPrefix, TopicListingCatalogNoNote) {
  const std::string text =
      "Loaded data (1 topic(s)):\n"
      "  /a/b: value (float64)\n"
      "\n\n"
      "what is loaded?";
  EXPECT_EQ(stripCatalogPrefix(text), "what is loaded?");
}

TEST(StripCatalogPrefix, TopicListingCatalogWithChangedNote) {
  const std::string text =
      "Loaded data (1 topic(s)):\n"
      "  /a/b: value (float64)\n"
      "\n"
      "(The loaded data changed; the listing above replaces the earlier one.)\n"
      "\n"
      "what changed?";
  EXPECT_EQ(stripCatalogPrefix(text), "what changed?");
}

TEST(StripCatalogPrefix, ShortCatalogWithResumedNote) {
  const std::string text =
      "Loaded data: nothing is loaded yet.\n"
      "(Resumed conversation. The tabs you composed earlier may no longer exist; plot_tab with action list reports the "
      "ones that do. The listing above is the data loaded "
      "now.)\n"
      "\n"
      "what tabs?";
  EXPECT_EQ(stripCatalogPrefix(text), "what tabs?");
}

TEST(StripCatalogPrefix, NotesAreRecognizedByTheirOpeningWordsAlone) {
  // A listing that ends with its own newline puts a blank line BETWEEN the
  // catalog and the note, so the note has to be recognized on its own — and
  // by its opening words only, so a session written with an earlier wording
  // of the sentence still strips cleanly.
  const std::string text =
      "Loaded data (1 topic(s)):\n"
      "  /imu: x (double)\n"
      "\n"
      "(Resumed conversation. Any tabs you composed earlier no longer exist. The listing above is the data loaded "
      "now.)\n"
      "\n"
      "what tabs?";
  EXPECT_EQ(stripCatalogPrefix(text), "what tabs?");
}

TEST(StripCatalogPrefix, MalformedCatalogWithNoBlankLineIsLeftAlone) {
  // Starts with the marker but never resolves to a blank line -- a truncated
  // record, say. Guessing here would risk eating real user text.
  EXPECT_EQ(
      stripCatalogPrefix("Loaded data (nothing else here, no blank line)"),
      "Loaded data (nothing else here, no blank line)");
}

// --- formatShortDate --------------------------------------------------------

TEST(ParseIso8601Utc, EpochSecondsAcrossLeapYearsAndCenturies) {
  EXPECT_EQ(parseIso8601Utc("1970-01-01T00:00:00.000Z"), 0);
  EXPECT_EQ(parseIso8601Utc("2000-03-01T00:00:00Z"), 951868800);  // the day after a century leap day
  EXPECT_EQ(parseIso8601Utc("2026-09-01T15:17:30.326Z"), 1788275850);
  EXPECT_FALSE(parseIso8601Utc("").has_value());
  EXPECT_FALSE(parseIso8601Utc("not a timestamp").has_value());
  EXPECT_FALSE(parseIso8601Utc("2026-13-01T00:00:00Z").has_value());
  // What the SDK parser gives that a fixed-offset read of the first 19
  // characters could not: the calendar is actually checked, and a numeric zone
  // is applied rather than assumed to be UTC.
  EXPECT_FALSE(parseIso8601Utc("2026-02-31T00:00:00Z").has_value());
  EXPECT_EQ(parseIso8601Utc("2026-09-01T17:17:30.326+02:00"), 1788275850);
}

TEST(FormatShortDate, FormatsInUtcWhenAsked) {
  EXPECT_EQ(formatShortDate("2026-09-01T15:17:30.326Z", /*local=*/false), "1 Sep 15:17");
  EXPECT_EQ(formatShortDate("2026-09-02T09:00:05.900Z", /*local=*/false), "2 Sep 09:00");
  EXPECT_EQ(formatShortDate("2026-12-31T23:59:59Z", /*local=*/false), "31 Dec 23:59");
}

TEST(FormatShortDate, EmptyOnUnparsable) {
  EXPECT_EQ(formatShortDate("", /*local=*/false), "");
  EXPECT_EQ(formatShortDate("not a timestamp", /*local=*/false), "");
}

TEST_F(ClaudeSessionsTest, FallbackTitleNeverSplitsAMultiByteCharacter) {
  // 47 ASCII bytes then a two-byte "ó": the 48-byte cut lands inside it and
  // must back off to the boundary instead of leaving half a code point.
  const std::string prompt = std::string(47, 'a') + "\xC3\xB3" + "tail";
  {
    std::ofstream file(dir_ / "session_accent.jsonl");
    file
        << R"({"type":"user","timestamp":"2026-09-02T10:00:00.000Z","sessionId":"session_accent","message":{"role":"user","content":")"
        << prompt << R"("}})"
        << "\n";
    file
        << R"({"type":"assistant","timestamp":"2026-09-02T10:00:05.000Z","sessionId":"session_accent","message":{"role":"assistant","content":[{"type":"text","text":"ok"}]}})"
        << "\n";
  }
  const auto convs = listConversations(dir_);
  const ConversationSummary* accent = findById(convs, "session_accent");
  ASSERT_NE(accent, nullptr);
  EXPECT_EQ(accent->title.size(), 47u);
  EXPECT_EQ(accent->title, std::string(47, 'a'));
}

// --- (a)/(b)/(e) listConversations: order, filter, title chain, corruption -

TEST_F(ClaudeSessionsTest, ListsNewestFirstAndExcludesSessionsWithNoAssistantReply) {
  const std::vector<ConversationSummary> convs = listConversations(dir_);
  // session_delta_no_assistant has zero `assistant` records (a cancelled
  // turn, or a smoke test run against this cwd) and must not appear.
  ASSERT_EQ(convs.size(), 5u);
  EXPECT_EQ(convs[0].id, "session_gamma");
  EXPECT_EQ(convs[1].id, "session_epsilon_corrupt");
  EXPECT_EQ(convs[2].id, "session_alpha");
  EXPECT_EQ(convs[3].id, "session_beta");
  EXPECT_EQ(convs[4].id, "session_zeta_untitled");
  EXPECT_EQ(findById(convs, "session_delta_no_assistant"), nullptr);
}

TEST_F(ClaudeSessionsTest, TitleResolutionChain) {
  const std::vector<ConversationSummary> convs = listConversations(dir_);

  // ai-title wins when present, even alongside a corrupt line and an unknown
  // record type elsewhere in the same file.
  ASSERT_NE(findById(convs, "session_alpha"), nullptr);
  EXPECT_EQ(findById(convs, "session_alpha")->title, "PlotJuggler demo");
  ASSERT_NE(findById(convs, "session_beta"), nullptr);
  EXPECT_EQ(findById(convs, "session_beta")->title, "PlotJuggler capabilities");
  ASSERT_NE(findById(convs, "session_epsilon_corrupt"), nullptr);
  EXPECT_EQ(findById(convs, "session_epsilon_corrupt")->title, "Resumed tabs question");

  // No ai-title: falls back to the first prompt, catalog stripped, cut to 48
  // chars.
  ASSERT_NE(findById(convs, "session_gamma"), nullptr);
  EXPECT_EQ(findById(convs, "session_gamma")->title, "Can you plot the vehicle speed against the steer");

  // Neither an ai-title nor any user text block (its one user record carries
  // only a tool_result): Untitled.
  ASSERT_NE(findById(convs, "session_zeta_untitled"), nullptr);
  EXPECT_EQ(findById(convs, "session_zeta_untitled")->title, "Untitled");
}

// --- (d) loadTranscript: row order, roles, tool formatting, tool_result ----

TEST_F(ClaudeSessionsTest, TranscriptOrdersUserAssistantAndToolRowsIgnoringToolResult) {
  const std::vector<ChatMessage> rows = loadTranscript(dir_, "session_gamma");
  ASSERT_EQ(rows.size(), 3u);

  EXPECT_EQ(rows[0].role, ChatMessage::Role::User);
  EXPECT_EQ(rows[0].text, "Can you plot the vehicle speed against the steering angle?");

  // tool_use -> a Tool row, "mcp__pj__" stripped -- the same text the live
  // BackendEvent::ToolActivity path renders (claude_backend.cpp), so a
  // replayed transcript reads exactly like it did live.
  EXPECT_EQ(rows[1].role, ChatMessage::Role::Tool);
  EXPECT_EQ(rows[1].text, "list_topics");

  // The tool_result that answered it carries no row of its own.
  EXPECT_EQ(rows[2].role, ChatMessage::Role::Assistant);
  EXPECT_EQ(rows[2].text, "I opened a new tab plotting vehicle_speed against vehicle_steering.");
}

TEST_F(ClaudeSessionsTest, TranscriptFromRealSessionStripsTheCatalogPrefix) {
  const std::vector<ChatMessage> alpha = loadTranscript(dir_, "session_alpha");
  ASSERT_EQ(alpha.size(), 2u);
  EXPECT_EQ(alpha[0].role, ChatMessage::Role::User);
  EXPECT_EQ(
      alpha[0].text,
      "Give me a demo showing me everything you’re capable of doing inside PlotJuggler, while showing me what "
      "you’re doing and the results as you go.\n");
  EXPECT_EQ(alpha[1].role, ChatMessage::Role::Assistant);
  EXPECT_EQ(alpha[1].text.rfind("I'd love to give you a demo", 0), 0u);

  const std::vector<ChatMessage> beta = loadTranscript(dir_, "session_beta");
  ASSERT_EQ(beta.size(), 2u);
  EXPECT_EQ(beta[0].role, ChatMessage::Role::User);
  EXPECT_EQ(beta[0].text, "Show all the things that can u do inside plotjuggler");
}

TEST_F(ClaudeSessionsTest, TranscriptSkipsACorruptLineAndAnUnknownRecordType) {
  const std::vector<ChatMessage> rows = loadTranscript(dir_, "session_epsilon_corrupt");
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].role, ChatMessage::Role::User);
  EXPECT_EQ(rows[0].text, "What tabs did you create earlier?");
  EXPECT_EQ(rows[1].role, ChatMessage::Role::Assistant);
  EXPECT_EQ(rows[1].text, "I don't have a record of tabs from before this resume.");
}

TEST_F(ClaudeSessionsTest, TranscriptForMissingIdIsEmpty) {
  EXPECT_TRUE(loadTranscript(dir_, "no-such-session").empty());
}

// --- (g) delete --------------------------------------------------------

TEST_F(ClaudeSessionsTest, DeleteRemovesFileAndIgnoresAbsence) {
  ASSERT_TRUE(std::filesystem::exists(dir_ / "session_alpha.jsonl"));
  EXPECT_TRUE(deleteConversation(dir_, "session_alpha"));
  EXPECT_FALSE(std::filesystem::exists(dir_ / "session_alpha.jsonl"));

  // Deleting an already-gone conversation is a no-op success, not a failure
  // the caller has to special-case (a double click, a race with retention).
  EXPECT_TRUE(deleteConversation(dir_, "session_alpha"));

  EXPECT_EQ(findById(listConversations(dir_), "session_alpha"), nullptr);
}

}  // namespace
