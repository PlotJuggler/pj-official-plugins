// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace mosaico {

constexpr std::int64_t kNsPerMs = 1'000'000LL;
constexpr std::int64_t kMsPerDay = 86'400'000LL;

// Time-of-day (ms since UTC midnight) for an epoch-ns instant.
inline int timeOfDayMs(std::int64_t epoch_ns) {
  std::int64_t ms = epoch_ns / kNsPerMs;
  std::int64_t tod = ms % kMsPerDay;
  if (tod < 0) {
    tod += kMsPerDay;
  }
  return static_cast<int>(tod);
}

// PJ3 SequencePicker RangeFilter semantics. A sequence spanning [seq_min,
// seq_max] (epoch-ns) passes when it intersects the picked range. Zero on a
// bound means "unbounded" on that side; a zero seq bound is treated as
// "unknown" and never excludes.
//
//   * every_day == false → simple [from_ns, to_ns] interval intersection.
//   * every_day == true  → additionally require the sequence to touch the
//     recurring daily time-of-day window [from_tod_ms, to_tod_ms]. A span of
//     >= 24h always covers the window; a sub-day arc that crosses midnight is
//     treated conservatively as a match (never hides a valid sequence).
inline bool dateFilterMatches(
    std::int64_t seq_min, std::int64_t seq_max, std::int64_t from_ns, std::int64_t to_ns, bool every_day,
    int from_tod_ms, int to_tod_ms) {
  // Interval intersection (unbounded side = 0).
  if (from_ns != 0 && seq_max != 0 && seq_max < from_ns) {
    return false;
  }
  if (to_ns != 0 && seq_min != 0 && seq_min > to_ns) {
    return false;
  }
  if (!every_day) {
    return true;
  }
  // Recurring daily window: clamp the sequence to the date range, then test
  // the clamped span's time-of-day arc against [from_tod_ms, to_tod_ms].
  std::int64_t a = seq_min;
  std::int64_t b = seq_max;
  if (from_ns != 0 && a < from_ns) {
    a = from_ns;
  }
  if (to_ns != 0 && b > to_ns) {
    b = to_ns;
  }
  if (a == 0 || b == 0 || a > b) {
    return a == 0 || b == 0;  // unknown bounds → don't exclude
  }
  if ((b - a) >= kMsPerDay * kNsPerMs) {
    return true;  // covers every time of day
  }
  const int tod_a = timeOfDayMs(a);
  const int tod_b = timeOfDayMs(b);
  if (tod_a <= tod_b) {
    // Arc does not cross midnight: [tod_a, tod_b] overlaps [from, to]?
    return tod_a <= to_tod_ms && from_tod_ms <= tod_b;
  }
  // Arc crosses midnight — wide; treat as a match (conservative).
  return true;
}

}  // namespace mosaico
