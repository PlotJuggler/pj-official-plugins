// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "usage_ledger.hpp"

#include <gtest/gtest.h>

namespace {

using assistant_agent::TurnMetrics;
using assistant_agent::UsageLedger;

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
  // Nothing reported: the label stays as statusText() left it.
  EXPECT_EQ(l.summary(), "");
}

// The summary is glued onto statusText(), so it carries its own separator; otherwise the label
// reads "Ready↑19.8k".
TEST(UsageLedger, SummaryStartsWithTheSeparator) {
  UsageLedger l;
  l.record(turn(10, 14508, 113));
  ASSERT_FALSE(l.summary().empty());
  EXPECT_EQ(l.summary().rfind(" - ", 0), 0u);
}

TEST(UsageLedger, SmallCountsStayExact) {
  UsageLedger l;
  l.record(turn(40, 0, 113));
  EXPECT_EQ(l.summary(), " - ↑40 ↓113");
}

// From a real recorded turn: 10 fresh input tokens on top of 14508 replayed from cache. Reporting
// the 10 alone would tell the user something false about a 14.5k prompt.
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

// A turn that died before the CLI's result record must not blank the last real turn's figures.
TEST(UsageLedger, FailedTurnLeavesTheSummaryStanding) {
  UsageLedger l;
  l.record(turn(10, 14508, 113));
  const std::string before = l.summary();
  l.record(TurnMetrics{});
  EXPECT_EQ(l.summary(), before);
}

// A conversation that only ever failed shows nothing, not "↑0 ↓0".
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
