// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <pj_transfer_rate/transfer_rate.hpp>

using PJ::common::RollingTransferRate;
using namespace std::chrono_literals;

TEST(TransferRate, MonotonicWindowCounterResetAndIdle) {
  RollingTransferRate rate;
  const RollingTransferRate::Clock::time_point start{};
  EXPECT_EQ(rate.bytesPerSecond(), 0.0);
  rate.add(100, start);
  rate.add(200, start);  // same timestamp is coalesced
  EXPECT_EQ(rate.bytesPerSecond(), 0.0);
  rate.add(400, start + 2s);
  EXPECT_DOUBLE_EQ(rate.bytesPerSecond(), 100.0);
  rate.add(900, start + 7s);
  EXPECT_DOUBLE_EQ(rate.bytesPerSecond(), 100.0);
  rate.add(900, start + 13s);
  EXPECT_EQ(rate.bytesPerSecond(), 0.0);
  rate.add(0, start + 14s);
  EXPECT_EQ(rate.bytesPerSecond(), 0.0);
  rate.add(100, start + 13s);
  EXPECT_EQ(rate.bytesPerSecond(), 0.0);
}
