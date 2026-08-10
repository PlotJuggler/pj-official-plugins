// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// ChatSession is the pure transcript + turn-state model the panel renders. No
// Qt, no threads — exhaustively testable in isolation.
#include "chat_session.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using assistant_agent::ChatSession;
using assistant_agent::TurnState;

TEST(ChatSession, StartsIdleAndEmpty) {
  ChatSession s;
  EXPECT_EQ(s.state(), TurnState::Idle);
  EXPECT_FALSE(s.busy());
  EXPECT_TRUE(s.messages().empty());
  EXPECT_EQ(s.render(), "");
}

TEST(ChatSession, RendersSpeakerTags) {
  ChatSession s;
  s.addUser("hello");
  s.addAssistant("hi there");
  const std::string out = s.render();
  EXPECT_NE(out.find("You: hello"), std::string::npos);
  EXPECT_NE(out.find("Assistant: hi there"), std::string::npos);
  // The user line comes before the assistant line.
  EXPECT_LT(out.find("You: hello"), out.find("Assistant: hi there"));
}

TEST(ChatSession, ToolAndSystemRowsRenderDistinctly) {
  ChatSession s;
  s.addTool("list_topics -> 12 topics");
  s.addSystem("Settings saved.");
  const std::string out = s.render();
  EXPECT_NE(out.find("list_topics"), std::string::npos);
  EXPECT_NE(out.find("Settings saved."), std::string::npos);
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
  EXPECT_EQ(s.statusText(), "Ready.");
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
  EXPECT_NE(s.render().find("You said: ping"), std::string::npos);
}

}  // namespace
