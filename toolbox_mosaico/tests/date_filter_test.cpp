// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Unit tests for the SequencePicker date filter (PJ3 RangeFilter interval parity).

#include "../src/date_filter.h"

#include "gtest/gtest.h"

using mosaico::dateFilterMatches;

namespace {
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;
constexpr std::int64_t kHourNs = 3'600'000'000'000LL;
constexpr std::int64_t kMidnight = 1000LL * kDayNs;  // an arbitrary UTC midnight
}  // namespace

TEST(DateFilter, IntervalIntersection) {
  const std::int64_t from = kMidnight;
  const std::int64_t to = kMidnight + kDayNs;
  // Sequence entirely before the range.
  EXPECT_FALSE(dateFilterMatches(from - 2 * kDayNs, from - kDayNs, from, to));
  // Sequence entirely after the range.
  EXPECT_FALSE(dateFilterMatches(to + kDayNs, to + 2 * kDayNs, from, to));
  // Overlapping sequence.
  EXPECT_TRUE(dateFilterMatches(from + kHourNs, from + 2 * kHourNs, from, to));
}

TEST(DateFilter, UnboundedSidesNeverExclude) {
  // from_ns == 0 and to_ns == 0 → no interval constraint.
  EXPECT_TRUE(dateFilterMatches(kMidnight, kMidnight + kHourNs, 0, 0));
}

TEST(DateFilter, UnknownTimestampShownOnlyWithoutActiveConstraint) {
  const std::int64_t from = kMidnight;
  const std::int64_t to = kMidnight + kDayNs;
  // No active constraint → a dateless sequence is shown (don't hide).
  EXPECT_TRUE(dateFilterMatches(0, 0, 0, 0));
  // Any active date range hides it (it can't be confirmed in-range — avoids the
  // "1970" false positives).
  EXPECT_FALSE(dateFilterMatches(0, 0, from, to));
  EXPECT_FALSE(dateFilterMatches(0, 0, from, 0));
  EXPECT_FALSE(dateFilterMatches(0, 0, 0, to));
}
