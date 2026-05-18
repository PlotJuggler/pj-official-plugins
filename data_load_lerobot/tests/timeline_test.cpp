#include "timeline.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using namespace lerobot;  // NOLINT(build/namespaces) — test-local convenience

TEST(ComputeEpisodeOffsetsNs, SingleEpisodeHasZeroOffset) {
  // Arrange / Act
  const auto offsets = computeEpisodeOffsetsNs({100}, 10.0, 0.0);

  // Assert — the user's "1 episode ⇒ nothing" falls out of the uniform rule
  ASSERT_EQ(offsets.size(), 1u);
  EXPECT_EQ(offsets[0], 0);
}

TEST(ComputeEpisodeOffsetsNs, ConcatenatesBackToBackNoGap) {
  // Arrange: 50 frames @ 10 fps = 5 s; 30 frames @ 10 fps = 3 s
  // Act
  const auto offsets = computeEpisodeOffsetsNs({50, 30, 20}, 10.0, 0.0);

  // Assert
  ASSERT_EQ(offsets.size(), 3u);
  EXPECT_EQ(offsets[0], 0);
  EXPECT_EQ(offsets[1], 5'000'000'000LL);          // after 5 s
  EXPECT_EQ(offsets[2], 5'000'000'000LL + 3'000'000'000LL);  // + 3 s
}

TEST(ComputeEpisodeOffsetsNs, AppliesInterEpisodeGap) {
  // Arrange / Act — 1 s gap between episodes
  const auto offsets = computeEpisodeOffsetsNs({50, 50}, 10.0, 1.0);

  // Assert: 5 s episode + 1 s gap = 6 s before episode 2
  ASSERT_EQ(offsets.size(), 2u);
  EXPECT_EQ(offsets[0], 0);
  EXPECT_EQ(offsets[1], 6'000'000'000LL);
}

TEST(ComputeEpisodeOffsetsNs, ClampsNonPositiveFps) {
  // Act — fps <= 0 must not divide-by-zero; clamps to 1.0
  const auto offsets = computeEpisodeOffsetsNs({2, 2}, 0.0, 0.0);

  // Assert: 2 frames @ 1 fps = 2 s
  ASSERT_EQ(offsets.size(), 2u);
  EXPECT_EQ(offsets[1], 2'000'000'000LL);
}

TEST(RowTimestampNs, UsesTimestampColumnWhenPresent) {
  EXPECT_EQ(rowTimestampNs(1'000'000'000LL, /*has_ts=*/true, 0.5, 999, 30.0),
            1'000'000'000LL + 500'000'000LL);
}

TEST(RowTimestampNs, FallsBackToFrameIndexOverFps) {
  // No timestamp column: frame 20 @ 10 fps = 2.0 s
  EXPECT_EQ(rowTimestampNs(0, /*has_ts=*/false, 0.0, 20, 10.0), 2'000'000'000LL);
}
