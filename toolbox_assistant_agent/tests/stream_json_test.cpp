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

TEST(ParseClaudeLine, MalformedIsIgnoredNotThrown) {
  EXPECT_TRUE(parseClaudeLine("not json").empty());
  EXPECT_TRUE(parseClaudeLine("").empty());
  EXPECT_TRUE(parseClaudeLine(R"({"type":"unknown"})").empty());
}

}  // namespace
