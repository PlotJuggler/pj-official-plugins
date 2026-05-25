// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Unit tests for the SequencePicker date/time filter (PJ3 RangeFilter parity),
// including the "Every day" recurring time-of-day window.

#include "../src/date_filter.h"

#include "gtest/gtest.h"

using mosaico::dateFilterMatches;
using mosaico::timeOfDayMs;

namespace {
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;
constexpr std::int64_t kHourNs = 3'600'000'000'000LL;
constexpr std::int64_t kMidnight = 1000LL * kDayNs;  // an arbitrary UTC midnight
constexpr int kH = 3'600'000;                        // one hour in ms
}  // namespace

TEST(DateFilter, TimeOfDayMidnightIsZero) {
  EXPECT_EQ(timeOfDayMs(kMidnight), 0);
  EXPECT_EQ(timeOfDayMs(kMidnight + 10 * kHourNs), 10 * kH);
}

TEST(DateFilter, IntervalIntersectionWhenNotEveryDay) {
  const std::int64_t from = kMidnight;
  const std::int64_t to = kMidnight + kDayNs;
  // Sequence entirely before the range.
  EXPECT_FALSE(dateFilterMatches(from - 2 * kDayNs, from - kDayNs, from, to, false, 0, 0));
  // Sequence entirely after the range.
  EXPECT_FALSE(dateFilterMatches(to + kDayNs, to + 2 * kDayNs, from, to, false, 0, 0));
  // Overlapping sequence.
  EXPECT_TRUE(dateFilterMatches(from + kHourNs, from + 2 * kHourNs, from, to, false, 0, 0));
}

TEST(DateFilter, UnboundedSidesNeverExclude) {
  // from_ns == 0 and to_ns == 0 → no interval constraint.
  EXPECT_TRUE(dateFilterMatches(kMidnight, kMidnight + kHourNs, 0, 0, false, 0, 0));
}

TEST(DateFilter, EveryDayInsideWindowMatches) {
  const std::int64_t from = kMidnight;
  const std::int64_t to = kMidnight + 7 * kDayNs;
  // Sequence at 10:00–11:00, window 09:00–17:00 → match.
  EXPECT_TRUE(dateFilterMatches(kMidnight + 10 * kHourNs, kMidnight + 11 * kHourNs, from, to, true, 9 * kH, 17 * kH));
}

TEST(DateFilter, EveryDayOutsideWindowExcluded) {
  const std::int64_t from = kMidnight;
  const std::int64_t to = kMidnight + 7 * kDayNs;
  // Sequence at 20:00–21:00, window 09:00–17:00 → excluded.
  EXPECT_FALSE(dateFilterMatches(kMidnight + 20 * kHourNs, kMidnight + 21 * kHourNs, from, to, true, 9 * kH, 17 * kH));
}

TEST(DateFilter, EveryDayPartialOverlapMatches) {
  const std::int64_t from = kMidnight;
  const std::int64_t to = kMidnight + 7 * kDayNs;
  // Sequence 08:00–18:00 overlaps the 09:00–17:00 window at the edges.
  EXPECT_TRUE(dateFilterMatches(kMidnight + 8 * kHourNs, kMidnight + 18 * kHourNs, from, to, true, 9 * kH, 17 * kH));
}

TEST(DateFilter, EveryDaySpanOver24hAlwaysCoversWindow) {
  const std::int64_t from = kMidnight;
  const std::int64_t to = kMidnight + 7 * kDayNs;
  // A 2-day sequence covers every time of day.
  EXPECT_TRUE(dateFilterMatches(kMidnight, kMidnight + 2 * kDayNs, from, to, true, 9 * kH, 17 * kH));
}

TEST(DateFilter, EveryDayClampedOutsideDateRangeExcluded) {
  // The 09:00–17:00 hit happens on a day OUTSIDE the date range → excluded by
  // the interval check before the time-of-day test even runs.
  const std::int64_t from = kMidnight + 3 * kDayNs;
  const std::int64_t to = kMidnight + 7 * kDayNs;
  EXPECT_FALSE(dateFilterMatches(kMidnight + 10 * kHourNs, kMidnight + 11 * kHourNs, from, to, true, 9 * kH, 17 * kH));
}
