// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// UsageLedger holds what the last turn moved and renders the tail of the status
// label. Pure — no Qt, no threads, no backend.
#include "usage_ledger.hpp"

#include <gtest/gtest.h>

namespace {

using assistant_agent::TurnMetrics;
using assistant_agent::UsageLedger;

// A turn as the CLI reports one: counted and flagged valid.
TurnMetrics turn(int input, int cache_read, int output) {
  TurnMetrics m;
  m.input_tokens = input;
  m.cache_read_tokens = cache_read;
  m.output_tokens = output;
  m.valid = true;
  return m;
}

TEST(UsageLedger, StartsEmptyAndSaysNothing) {
  UsageLedger l;
  EXPECT_TRUE(l.empty());
  // Nothing reported, nothing to say — the label stays as statusText() left it,
  // which is the whole story for a local backend.
  EXPECT_EQ(l.summary(), "");
}

// The summary is glued straight onto statusText(), so it has to carry its own
// separator — without it the label reads "Ready↑19.8k".
TEST(UsageLedger, SummaryStartsWithTheSeparator) {
  UsageLedger l;
  l.record(turn(10, 14508, 113));
  ASSERT_FALSE(l.summary().empty());
  EXPECT_EQ(l.summary().rfind(" - ", 0), 0u);
}

// Under a thousand the exact figure is short enough to show as-is.
TEST(UsageLedger, SmallCountsStayExact) {
  UsageLedger l;
  l.record(turn(40, 0, 113));
  EXPECT_EQ(l.summary(), " - ↑40 ↓113");
}

// The figure that matters, taken from a real recorded turn: 10 fresh input
// tokens on top of 14508 replayed from cache. Reporting the 10 alone would be a
// true number that tells the user something false about a 14.5k prompt.
TEST(UsageLedger, SentCountsTheCachedHistoryToo) {
  UsageLedger l;
  l.record(turn(10, 14508, 113));
  EXPECT_EQ(l.summary(), " - ↑14.5k ↓113");
}

TEST(UsageLedger, CacheCreationCountsAsSent) {
  UsageLedger l;
  TurnMetrics m = turn(10, 1000, 50);
  m.cache_creation_tokens = 2000;
  l.record(m);
  EXPECT_EQ(l.summary(), " - ↑3.0k ↓50");
}

TEST(UsageLedger, ShowsTheMostRecentTurn) {
  UsageLedger l;
  l.record(turn(10, 14508, 113));
  l.record(turn(10, 20000, 200));
  EXPECT_EQ(l.summary(), " - ↑20.0k ↓200");
}

// A turn that died before the CLI emitted its result record has nothing to
// report, and must not blank out the figures of the last real one.
TEST(UsageLedger, FailedTurnLeavesTheSummaryStanding) {
  UsageLedger l;
  l.record(turn(10, 14508, 113));
  const std::string before = l.summary();
  l.record(TurnMetrics{});  // valid defaults to false
  EXPECT_EQ(l.summary(), before);
}

// A conversation that only ever failed shows nothing rather than "↑0 ↓0".
TEST(UsageLedger, StaysEmptyWhenEveryTurnFailed) {
  UsageLedger l;
  l.record(TurnMetrics{});
  EXPECT_TRUE(l.empty());
  EXPECT_EQ(l.summary(), "");
}

TEST(UsageLedger, ResetReturnsToEmpty) {
  UsageLedger l;
  l.record(turn(10, 14508, 113));
  l.reset();
  EXPECT_TRUE(l.empty());
  EXPECT_EQ(l.summary(), "");
}

}  // namespace
