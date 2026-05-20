#include "timeline.hpp"

#include <cstdint>

#include <gtest/gtest.h>

using namespace lerobot;  // NOLINT(build/namespaces) — test-local convenience

TEST(RowTimestampNs, UsesTimestampColumnWhenPresent) {
  // Parquet ts=0.5 s → 500_000_000 ns on the episode's 0-based clock.
  EXPECT_EQ(rowTimestampNs(/*has_ts=*/true, 0.5, /*frame_index=*/999, 30.0), 500'000'000LL);
}

TEST(RowTimestampNs, FallsBackToFrameIndexOverFps) {
  // No timestamp column: frame 20 @ 10 fps = 2.0 s
  EXPECT_EQ(rowTimestampNs(/*has_ts=*/false, 0.0, 20, 10.0), 2'000'000'000LL);
}

TEST(RowTimestampNs, ClampsNonPositiveFps) {
  // Defensive: fps <= 0 must not divide-by-zero; clamps to 1 fps.
  EXPECT_EQ(rowTimestampNs(/*has_ts=*/false, 0.0, /*frame_index=*/3, 0.0), 3'000'000'000LL);
}
