// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#include "toolbox_preview/adaptive_poll.hpp"

#include <gtest/gtest.h>

#include <functional>

using toolbox_preview::AdaptivePoll;

namespace {

// Drive `ticks` panel ticks through the poll. On every tick we admit a read iff
// shouldRead(); when we read, `changed(read_index)` decides whether it observed a change,
// which we feed back via observe(). Returns how many reads actually happened.
int simulate(AdaptivePoll& poll, int ticks, const std::function<bool(int)>& changed) {
  int reads = 0;
  for (int t = 0; t < ticks; ++t) {
    if (poll.shouldRead()) {
      poll.observe(changed(reads));
      ++reads;
    }
  }
  return reads;
}

}  // namespace

TEST(AdaptivePollTest, EagerByDefault) {
  AdaptivePoll poll;
  EXPECT_TRUE(poll.shouldRead());
}

TEST(AdaptivePollTest, MovingSourceReadsEveryTick) {
  AdaptivePoll poll;
  // A source that changes on every read must never be throttled — smooth streaming.
  EXPECT_EQ(simulate(poll, 100, [](int) { return true; }), 100);
}

TEST(AdaptivePollTest, StaticSourceBacksOff) {
  AdaptivePoll poll;
  // A source that never changes should end up reading far less than once per tick.
  const int reads = simulate(poll, 100, [](int) { return false; });
  EXPECT_GT(reads, 0);   // it still polls occasionally (to notice a future change)
  EXPECT_LT(reads, 40);  // but markedly fewer than the 100 ticks
}

TEST(AdaptivePollTest, ChangeSnapsBackToEveryTick) {
  AdaptivePoll poll;
  simulate(poll, 60, [](int) { return false; });  // back off
  while (!poll.shouldRead()) {                    // drain any pending skip gap
  }
  poll.observe(true);              // a change is observed
  EXPECT_TRUE(poll.shouldRead());  // -> immediately eager again
}

TEST(AdaptivePollTest, ResetReturnsToEager) {
  AdaptivePoll poll;
  simulate(poll, 60, [](int) { return false; });  // back off
  poll.reset();
  EXPECT_TRUE(poll.shouldRead());
}
