// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "series_stats.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using assistant_agent::bucketize;
using assistant_agent::computeStats;

constexpr std::int64_t kSec = 1'000'000'000;  // ns per second

TEST(SeriesStats, EmptyIsZeroed) {
  auto s = computeStats({}, {});
  EXPECT_EQ(s.count, 0u);
  EXPECT_EQ(s.rate_hz, 0.0);
}

TEST(SeriesStats, BasicStats) {
  std::vector<std::int64_t> ts = {0, kSec, 2 * kSec, 3 * kSec, 4 * kSec};
  std::vector<double> v = {1.0, 2.0, 3.0, 4.0, 5.0};
  auto s = computeStats(ts, v);
  EXPECT_EQ(s.count, 5u);
  EXPECT_DOUBLE_EQ(s.min, 1.0);
  EXPECT_DOUBLE_EQ(s.max, 5.0);
  EXPECT_DOUBLE_EQ(s.mean, 3.0);
  EXPECT_DOUBLE_EQ(s.duration_s, 4.0);
  // 4 intervals over 4 s -> 1 Hz.
  EXPECT_DOUBLE_EQ(s.rate_hz, 1.0);
  EXPECT_NEAR(s.stddev, 1.4142135623730951, 1e-9);
}

TEST(SeriesStats, SingleSampleHasNoRate) {
  std::vector<std::int64_t> ts = {42};
  std::vector<double> v = {7.0};
  auto s = computeStats(ts, v);
  EXPECT_EQ(s.count, 1u);
  EXPECT_DOUBLE_EQ(s.min, 7.0);
  EXPECT_DOUBLE_EQ(s.max, 7.0);
  EXPECT_EQ(s.rate_hz, 0.0);
}

TEST(SeriesBuckets, FewerSamplesThanBucketsOnePer) {
  std::vector<std::int64_t> ts = {0, kSec, 2 * kSec};
  std::vector<double> v = {1.0, 2.0, 3.0};
  auto b = bucketize(ts, v, 10);
  ASSERT_EQ(b.size(), 3u);
  EXPECT_DOUBLE_EQ(b[0].t_rel_s, 0.0);
  EXPECT_DOUBLE_EQ(b[2].t_rel_s, 2.0);
  EXPECT_DOUBLE_EQ(b[1].mean, 2.0);
}

TEST(SeriesBuckets, PreservesSpikeMinMax) {
  // 100 samples, a single sharp spike at index 50. A stride-decimation to 10
  // points could skip it; min/max-preserving buckets must keep it.
  std::vector<std::int64_t> ts;
  std::vector<double> v;
  for (int i = 0; i < 100; ++i) {
    ts.push_back(static_cast<std::int64_t>(i) * kSec / 10);
    v.push_back(i == 50 ? 999.0 : 0.0);
  }
  auto b = bucketize(ts, v, 10);
  ASSERT_FALSE(b.empty());
  double global_max = 0.0;
  std::size_t total = 0;
  for (const auto& bucket : b) {
    global_max = std::max(global_max, bucket.max);
    total += bucket.count;
  }
  EXPECT_DOUBLE_EQ(global_max, 999.0);  // the spike survived
  EXPECT_EQ(total, 100u);               // every sample landed in exactly one bucket
  EXPECT_LE(b.size(), 10u);
}

TEST(SeriesBuckets, DegenerateTimeSpan) {
  // All samples share a timestamp -> one bucket per sample, capped at max_points.
  std::vector<std::int64_t> ts = {5, 5, 5, 5};
  std::vector<double> v = {1.0, 2.0, 3.0, 4.0};
  auto b = bucketize(ts, v, 2);
  EXPECT_EQ(b.size(), 2u);
}

}  // namespace
