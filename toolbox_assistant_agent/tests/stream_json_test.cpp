// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "stream_json.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using assistant_agent::ClaudeEvent;
using assistant_agent::NdjsonSplitter;
using assistant_agent::parseClaudeLine;

TEST(NdjsonSplitter, SplitsAndBuffersPartialLines) {
  NdjsonSplitter s;
  std::vector<std::string> lines;
  auto sink = [&](const std::string& l) { lines.push_back(l); };
  s.feed("{\"a\":1}\n{\"b\"", sink);  // one complete line + a partial
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "{\"a\":1}");
  s.feed(":2}\n", sink);  // completes the buffered line
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1], "{\"b\":2}");
}

TEST(NdjsonSplitter, FlushEmitsTrailingUnterminatedLine) {
  NdjsonSplitter s;
  std::vector<std::string> lines;
  auto sink = [&](const std::string& l) { lines.push_back(l); };
  s.feed("{\"x\":1}", sink);  // no newline
  EXPECT_TRUE(lines.empty());
  s.flush(sink);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "{\"x\":1}");
}

TEST(ParseClaudeLine, InitCapturesSessionId) {
  auto evs = parseClaudeLine(R"({"type":"system","subtype":"init","session_id":"abc123"})");
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, ClaudeEvent::Kind::Init);
  EXPECT_EQ(evs[0].session_id, "abc123");
}

TEST(ParseClaudeLine, AssistantTextAndToolUse) {
  const std::string line = R"({"type":"assistant","session_id":"s","message":{"content":[)"
                           R"({"type":"text","text":"Let me check."},)"
                           R"({"type":"tool_use","name":"mcp__pj__list_topics","input":{}}]}})";
  auto evs = parseClaudeLine(line);
  ASSERT_EQ(evs.size(), 2u);
  EXPECT_EQ(evs[0].kind, ClaudeEvent::Kind::AssistantText);
  EXPECT_EQ(evs[0].text, "Let me check.");
  EXPECT_EQ(evs[1].kind, ClaudeEvent::Kind::ToolUse);
  EXPECT_EQ(evs[1].tool_name, "mcp__pj__list_topics");
}

TEST(ParseClaudeLine, ResultSuccess) {
  auto evs =
      parseClaudeLine(R"({"type":"result","subtype":"success","result":"Done.","session_id":"s","is_error":false})");
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, ClaudeEvent::Kind::Result);
  EXPECT_FALSE(evs[0].is_error);
  EXPECT_EQ(evs[0].text, "Done.");
}

TEST(ParseClaudeLine, ResultErrorSubtypeMarksError) {
  auto evs = parseClaudeLine(R"({"type":"result","subtype":"error_during_execution","result":"boom","is_error":true})");
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, ClaudeEvent::Kind::Result);
  EXPECT_TRUE(evs[0].is_error);
}

// The usage-window status is a family, not a flag. Treating every value that is
// not exactly "allowed" as a block once stopped a batch run that had plenty of
// window left, on nothing worse than a warning.
TEST(ParseClaudeLine, RateLimitAllowedFamilyIsNotAnError) {
  for (const char* status : {"allowed", "allowed_warning"}) {
    const std::string line =
        std::string(R"({"type":"rate_limit_event","rate_limit_info":{"status":")") + status + R"("}})";
    auto evs = parseClaudeLine(line);
    ASSERT_EQ(evs.size(), 1u) << status;
    EXPECT_EQ(evs[0].kind, ClaudeEvent::Kind::RateLimit) << status;
    EXPECT_FALSE(evs[0].is_error) << status << " still permits requests";
  }
}

TEST(ParseClaudeLine, RateLimitRejectedIsAnError) {
  auto evs = parseClaudeLine(R"({"type":"rate_limit_event","rate_limit_info":{"status":"rejected"}})");
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, ClaudeEvent::Kind::RateLimit);
  EXPECT_TRUE(evs[0].is_error);
  EXPECT_EQ(evs[0].text, "rejected");
}

TEST(ParseClaudeLine, ResultCarriesTheTurnCountAndToleratesANullResult) {
  // Verbatim shape of what the CLI emits for `--resume <unknown id>`: an
  // error, zero turns, and `"result": null` rather than a string.
  auto evs = parseClaudeLine(
      R"({"type":"result","subtype":"error_during_execution","is_error":true,"num_turns":0,"result":null,"session_id":"gone"})");
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_TRUE(evs[0].is_error);
  EXPECT_EQ(evs[0].num_turns, 0);
  EXPECT_EQ(evs[0].text, "");

  auto ok = parseClaudeLine(R"({"type":"result","subtype":"success","result":"Done.","num_turns":3})");
  ASSERT_EQ(ok.size(), 1u);
  EXPECT_EQ(ok[0].num_turns, 3);

  auto silent = parseClaudeLine(R"({"type":"result","subtype":"success","result":"Done."})");
  ASSERT_EQ(silent.size(), 1u);
  EXPECT_EQ(silent[0].num_turns, -1) << "absent means unknown, not zero";
}

// Verbatim (trimmed) shape of what the CLI emits when it never reaches the
// API at all -- an unrecognized --model value, verified live. Neither
// `is_error` nor `subtype` is set on the real line itself; `terminal_reason`
// paired with an all-zero (invalid) usage block is the only signal, so the
// parser folds that pairing into `is_error` (the same "business rule in the
// parser" precedent as the rate-limit event) -- ClaudeBackend then only has
// to react to is_error + terminal_reason, not re-derive the condition.
TEST(ParseClaudeLine, ResultCarriesTerminalReasonAndAllZeroUsageIsInvalidMetrics) {
  auto evs = parseClaudeLine(
      R"({"duration_api_ms":0,"stop_reason":"stop_sequence",)"
      R"("session_id":"59c13c03-bd19-4994-a4d0-ca9016a2d7ae","total_cost_usd":0,)"
      R"("usage":{"input_tokens":0,"cache_creation_input_tokens":0,)"
      R"("cache_read_input_tokens":0,"output_tokens":0},"terminal_reason":"api_error"})");
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].kind, ClaudeEvent::Kind::Result);
  EXPECT_EQ(evs[0].terminal_reason, "api_error");
  EXPECT_TRUE(evs[0].is_error) << "api_error + invalid metrics is folded into is_error by the parser";
  EXPECT_FALSE(evs[0].metrics.valid) << "an all-zero usage/cost/duration record must not look like a real turn";
  EXPECT_EQ(evs[0].metrics.input_tokens, 0);
  EXPECT_EQ(evs[0].metrics.output_tokens, 0);
}

TEST(ParseClaudeLine, TerminalReasonIsEmptyWhenAbsent) {
  auto evs = parseClaudeLine(R"({"type":"result","subtype":"success","result":"Done."})");
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].terminal_reason, "");
}

TEST(ParseClaudeLine, MalformedIsIgnoredNotThrown) {
  EXPECT_TRUE(parseClaudeLine("not json").empty());
  EXPECT_TRUE(parseClaudeLine("").empty());
  EXPECT_TRUE(parseClaudeLine(R"({"type":"unknown"})").empty());
}

}  // namespace
