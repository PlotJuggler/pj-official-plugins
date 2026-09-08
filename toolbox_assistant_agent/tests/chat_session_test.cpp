// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// ChatSession is the pure transcript + turn-state model the panel renders. No
// Qt, no threads — exhaustively testable in isolation.
#include "chat_session.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using assistant_agent::ChatMessage;
using assistant_agent::ChatSession;
using assistant_agent::TurnState;

TEST(ChatSession, StartsIdleAndEmpty) {
  ChatSession s;
  EXPECT_EQ(s.state(), TurnState::Idle);
  EXPECT_FALSE(s.busy());
  EXPECT_TRUE(s.messages().empty());
  EXPECT_EQ(s.renderMarkdown(), "");
}

TEST(ChatSession, RendersSpeakerTags) {
  ChatSession s;
  s.addUser("hello");
  s.addAssistant("hi there");
  const std::string out = s.renderMarkdown();
  EXPECT_NE(out.find("**You:**\n\nhello"), std::string::npos);
  EXPECT_NE(out.find("**Assistant:**\n\nhi there"), std::string::npos);
  // The user line comes before the assistant line.
  EXPECT_LT(out.find("**You:**"), out.find("**Assistant:**"));
}

TEST(ChatSession, ToolAndSystemRowsRenderDistinctly) {
  ChatSession s;
  s.addTool("list_topics -> 12 topics");
  s.addSystem("Settings saved.");
  const std::string out = s.renderMarkdown();
  // Tool/system bodies are Markdown-escaped: '_', '-', '>' and '.' are punctuation.
  EXPECT_NE(out.find("**Tool:**\n\nlist\\_topics \\-\\> 12 topics"), std::string::npos);
  EXPECT_NE(out.find("**System:**\n\nSettings saved\\."), std::string::npos);
}

TEST(ChatSession, MarkdownKeepsRoleLabelsSeparateFromAuthoredBlocks) {
  ChatSession s;
  s.addUser("# Inspect\n\n- speed\n- steering");
  s.addAssistant("1. First\n2. Second");

  EXPECT_EQ(
      s.renderMarkdown(), "**You:**\n\n# Inspect\n\n- speed\n- steering\n\n**Assistant:**\n\n1. First\n2. Second\n\n");
  EXPECT_EQ(s.messages()[0].text, "# Inspect\n\n- speed\n- steering") << "rendering must not rewrite stored messages";
}

TEST(ChatSession, MarkdownEscapesToolAndSystemText) {
  ChatSession s;
  s.addTool("read_series `speed` -> [1, 2]");
  s.addSystem("Error: <untrusted> **literal**");

  const std::string out = s.renderMarkdown();
  EXPECT_NE(out.find(R"(read\_series \`speed\` \-\> \[1\, 2\])"), std::string::npos);
  EXPECT_NE(out.find(R"(Error\: \<untrusted\> \*\*literal\*\*)"), std::string::npos);
}

TEST(ChatSession, MarkdownIsolatesClosedAndUnclosedFencesAcrossMessages) {
  ChatSession s;
  s.addAssistant("```cpp\nint complete;\n```");
  s.addUser("after closed fence");
  s.addAssistant("~~~~ lua\nreturn 1");
  s.addUser("after unfinished fence");

  const std::string out = s.renderMarkdown();
  EXPECT_NE(out.find("```cpp\nint complete;\n```\n\n**You:**"), std::string::npos);
  EXPECT_NE(out.find("~~~~ lua\nreturn 1\n~~~~\n\n**You:**"), std::string::npos);
}

TEST(ChatSession, MarkdownRecognizesCrLfFenceClosures) {
  ChatSession s;
  s.addAssistant("```text\r\ncomplete\r\n```\r\n");
  s.addSystem("- literal item\n1. literal number");

  const std::string out = s.renderMarkdown();
  EXPECT_EQ(out.find("```\n\n**System:**"), std::string::npos) << "a closed CRLF fence must not gain another close";
  EXPECT_NE(out.find("\\- literal item\n1\\. literal number"), std::string::npos);
}

TEST(ChatSession, MarkdownClosesOnlyTheCurrentStreamingSnapshot) {
  ChatSession s;
  s.appendAssistant("```json\n{");
  EXPECT_EQ(s.renderMarkdown(), "**Assistant:**\n\n```json\n{\n```\n\n");

  s.appendAssistant("}\n```");
  EXPECT_EQ(s.messages().front().text, "```json\n{}\n```") << "the temporary closure must not enter stored text";
  EXPECT_EQ(s.renderMarkdown(), "**Assistant:**\n\n```json\n{}\n```\n\n");
}

TEST(ChatSession, BusyReflectsNonIdleStates) {
  ChatSession s;
  EXPECT_FALSE(s.busy());
  s.setState(TurnState::WaitingForLlm);
  EXPECT_TRUE(s.busy());
  s.setState(TurnState::ExecutingTool);
  EXPECT_TRUE(s.busy());
  s.setState(TurnState::Idle);
  EXPECT_FALSE(s.busy());
}

TEST(ChatSession, StatusTextTracksState) {
  ChatSession s;
  EXPECT_EQ(s.statusText(), "Ready");
  s.setState(TurnState::WaitingForLlm);
  EXPECT_EQ(s.statusText(), "Thinking…");
  s.setState(TurnState::ExecutingTool);
  EXPECT_EQ(s.statusText(), "Running a tool…");
}

TEST(ChatSession, ClearResetsEverything) {
  ChatSession s;
  s.addUser("x");
  s.setState(TurnState::WaitingForLlm);
  s.clear();
  EXPECT_TRUE(s.messages().empty());
  EXPECT_EQ(s.state(), TurnState::Idle);
}

// Simulates the echo turn lifecycle: user sends -> WaitingForLlm ->
// assistant reply -> back to Idle. This is the exact sequence the dialog's
// worker/event plumbing produces in M1.
TEST(ChatSession, EchoTurnLifecycle) {
  ChatSession s;
  s.addUser("ping");
  s.setState(TurnState::WaitingForLlm);
  EXPECT_TRUE(s.busy());

  s.addAssistant("You said: ping");
  s.setState(TurnState::Idle);
  EXPECT_FALSE(s.busy());
  EXPECT_EQ(s.messages().size(), 2u);
  EXPECT_NE(s.renderMarkdown().find("You said: ping"), std::string::npos);
}

// Restoring a persisted conversation replays messages() through the addX
// methods (assistant_dialog's loadPersistedConversation) — the rebuilt session
// must render byte-identically to the original, rows and speaker tags intact.
TEST(ChatSession, ReplayingMessagesReproducesTheRender) {
  ChatSession original;
  original.addUser("how many topics?");
  original.addTool("list_topics");
  original.addAssistant("6 topics.");
  original.addSystem("note");
  original.addAssistant("Anything else?");  // consecutive assistant rows stay separate

  ChatSession restored;
  for (const auto& m : original.messages()) {
    switch (m.role) {
      case ChatMessage::Role::User:
        restored.addUser(m.text);
        break;
      case ChatMessage::Role::Assistant:
        restored.addAssistant(m.text);
        break;
      case ChatMessage::Role::System:
        restored.addSystem(m.text);
        break;
      case ChatMessage::Role::Tool:
        restored.addTool(m.text);
        break;
    }
  }
  EXPECT_EQ(restored.renderMarkdown(), original.renderMarkdown());
  EXPECT_EQ(restored.messages().size(), original.messages().size());
}

}  // namespace
