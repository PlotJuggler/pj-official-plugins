// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "series_stats.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace {

using assistant_agent::bucketize;
using assistant_agent::computeStats;

constexpr std::int64_t kSec = 1'000'000'000;  // ns per second
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kPosInf = std::numeric_limits<double>::infinity();
constexpr double kNegInf = -std::numeric_limits<double>::infinity();

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

// max_at_s/min_at_s must report the FIRST occurrence: the max repeats at
// t=3s but has to stay pinned at t=1s.
TEST(SeriesStats, MinMaxAtSReportFirstOccurrence) {
  std::vector<std::int64_t> ts = {0, kSec, 2 * kSec, 3 * kSec, 4 * kSec};
  std::vector<double> v = {2.0, 5.0, 1.0, 5.0, 3.0};
  auto s = computeStats(ts, v);
  EXPECT_DOUBLE_EQ(s.max, 5.0);
  EXPECT_DOUBLE_EQ(s.max_at_s, 1.0);
  EXPECT_DOUBLE_EQ(s.min, 1.0);
  EXPECT_DOUBLE_EQ(s.min_at_s, 2.0);
}

TEST(SeriesStats, SingleSampleHasNoRate) {
  std::vector<std::int64_t> ts = {42};
  std::vector<double> v = {7.0};
  auto s = computeStats(ts, v);
  EXPECT_EQ(s.count, 1u);
  EXPECT_DOUBLE_EQ(s.min, 7.0);
  EXPECT_DOUBLE_EQ(s.max, 7.0);
  EXPECT_EQ(s.rate_hz, 0.0);
  EXPECT_FALSE(s.has_gap);  // one sample has no spacing to report
}

// The property that makes the gap fields necessary: this series and a
// perfectly regular one have the same count, mean and a near-identical rate,
// so nothing else in the report can betray the dropout.
TEST(SeriesStats, LargestGapAndItsPosition) {
  // 1 Hz samples with one 3 s hole after t=2 s: 0,1,2,[hole],5,6.
  std::vector<std::int64_t> ts = {0, kSec, 2 * kSec, 5 * kSec, 6 * kSec};
  std::vector<double> v = {1.0, 1.0, 1.0, 1.0, 1.0};
  auto s = computeStats(ts, v);
  ASSERT_TRUE(s.has_gap);
  EXPECT_DOUBLE_EQ(s.max_gap_s, 3.0);
  EXPECT_DOUBLE_EQ(s.max_gap_at_s, 2.0);
}

TEST(SeriesStats, GapPositionIsRelativeToTheFirstSample) {
  // Same shape, shifted to start at t=100 s: the gap after the FIRST interval,
  // reported on the buckets' relative time axis (0 = first sample).
  std::vector<std::int64_t> ts = {100 * kSec, 104 * kSec, 105 * kSec};
  std::vector<double> v = {1.0, 1.0, 1.0};
  auto s = computeStats(ts, v);
  ASSERT_TRUE(s.has_gap);
  EXPECT_DOUBLE_EQ(s.max_gap_s, 4.0);
  EXPECT_DOUBLE_EQ(s.max_gap_at_s, 0.0);
}

TEST(SeriesStats, RegularSeriesReportsItsSpacingAsTheGap) {
  // No hole: the max gap IS the nominal spacing. Reporting it anyway lets the
  // reader see "largest gap == 1/rate" and conclude health from facts.
  std::vector<std::int64_t> ts = {0, kSec, 2 * kSec, 3 * kSec};
  std::vector<double> v = {0.0, 0.0, 0.0, 0.0};
  auto s = computeStats(ts, v);
  ASSERT_TRUE(s.has_gap);
  EXPECT_DOUBLE_EQ(s.max_gap_s, 1.0);
}

TEST(SeriesStats, OneNaNIsExcludedFromMinMaxMeanStddev) {
  std::vector<std::int64_t> ts = {0, kSec, 2 * kSec, 3 * kSec, 4 * kSec};
  std::vector<double> v = {1.0, 2.0, kNaN, 4.0, 5.0};
  auto s = computeStats(ts, v);
  EXPECT_EQ(s.count, 5u);  // total samples, NaN included
  EXPECT_EQ(s.invalid, 1u);
  ASSERT_TRUE(s.has_values);
  // Same min/max/mean/stddev as the four finite values {1,2,4,5}.
  EXPECT_DOUBLE_EQ(s.min, 1.0);
  EXPECT_DOUBLE_EQ(s.max, 5.0);
  EXPECT_DOUBLE_EQ(s.mean, 3.0);
  EXPECT_NEAR(s.stddev, 1.5811388300841898, 1e-9);
}

TEST(SeriesStats, AllNaNHasNoValues) {
  std::vector<std::int64_t> ts = {0, kSec, 2 * kSec};
  std::vector<double> v = {kNaN, kNaN, kNaN};
  auto s = computeStats(ts, v);
  EXPECT_EQ(s.count, 3u);
  EXPECT_EQ(s.invalid, 3u);
  EXPECT_FALSE(s.has_values);
  // min/max/mean/stddev stay at their defaults rather than becoming NaN.
  EXPECT_DOUBLE_EQ(s.min, 0.0);
  EXPECT_DOUBLE_EQ(s.max, 0.0);
  EXPECT_DOUBLE_EQ(s.mean, 0.0);
  EXPECT_DOUBLE_EQ(s.stddev, 0.0);
  // Duration/rate describe spacing, not values, so they are unaffected.
  EXPECT_DOUBLE_EQ(s.duration_s, 2.0);
  EXPECT_DOUBLE_EQ(s.rate_hz, 1.0);
}

TEST(SeriesStats, InfinitiesAreInvalidToo) {
  std::vector<std::int64_t> ts = {0, kSec, 2 * kSec};
  std::vector<double> v = {kPosInf, 1.0, kNegInf};
  auto s = computeStats(ts, v);
  EXPECT_EQ(s.invalid, 2u);
  ASSERT_TRUE(s.has_values);
  EXPECT_DOUBLE_EQ(s.min, 1.0);
  EXPECT_DOUBLE_EQ(s.max, 1.0);
  EXPECT_DOUBLE_EQ(s.mean, 1.0);
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

TEST(SeriesBuckets, NaNInABucketIsExcludedFromMinMaxMean) {
  // 100 samples over one bucket window (max_points = 1), one of them NaN.
  std::vector<std::int64_t> ts;
  std::vector<double> v;
  for (int i = 0; i < 100; ++i) {
    ts.push_back(static_cast<std::int64_t>(i) * kSec / 10);
    v.push_back(i == 50 ? kNaN : 1.0);
  }
  auto b = bucketize(ts, v, 1);
  ASSERT_EQ(b.size(), 1u);
  EXPECT_EQ(b[0].count, 99u);
  EXPECT_EQ(b[0].invalid, 1u);
  EXPECT_DOUBLE_EQ(b[0].min, 1.0);
  EXPECT_DOUBLE_EQ(b[0].max, 1.0);
  EXPECT_DOUBLE_EQ(b[0].mean, 1.0);
}

TEST(SeriesBuckets, BucketOfOnlyNaNHasZeroCount) {
  std::vector<std::int64_t> ts = {0, kSec, 2 * kSec};
  std::vector<double> v = {kNaN, kNaN, kNaN};
  auto b = bucketize(ts, v, 1);
  ASSERT_EQ(b.size(), 1u);
  EXPECT_EQ(b[0].count, 0u);
  EXPECT_EQ(b[0].invalid, 3u);
  EXPECT_DOUBLE_EQ(b[0].min, 0.0);
  EXPECT_DOUBLE_EQ(b[0].max, 0.0);
  EXPECT_DOUBLE_EQ(b[0].mean, 0.0);
}

}  // namespace
