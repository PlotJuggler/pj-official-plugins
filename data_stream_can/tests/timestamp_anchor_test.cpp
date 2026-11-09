#include "timestamp_anchor.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using can_stream::TimestampAnchor;

constexpr std::int64_t kHost = 1'700'000'000'000'000'000;  // 2023-11-14, a plausible host clock
constexpr std::int64_t kSecond = 1'000'000'000;

TEST(TimestampAnchor, ZeroDeviceTimeUsesHostTime) {
  TimestampAnchor anchor;
  EXPECT_EQ(anchor.map(0, 5'000'000'000'000'000'000), 5'000'000'000'000'000'000);
}

TEST(TimestampAnchor, AbsoluteDeviceTimePassesThrough) {
  TimestampAnchor anchor;
  EXPECT_EQ(anchor.map(1'700'000'000'000'000'000, 1'700'000'000'000'000'500), 1'700'000'000'000'000'000);
}

TEST(TimestampAnchor, RelativeDeviceTimeIsAnchoredOnFirstFrame) {
  TimestampAnchor anchor;
  EXPECT_EQ(anchor.map(kSecond, kHost), kHost);
  // The device clock, not the (jittery) host arrival time, spaces the later frames.
  EXPECT_EQ(anchor.map(kSecond + 1000, kHost + 999'999), kHost + 1000);
}

TEST(TimestampAnchor, BackwardJumpReanchors) {
  TimestampAnchor anchor;
  anchor.map(10 * kSecond, kHost);
  const std::int64_t host2 = kHost + 60 * kSecond;
  EXPECT_EQ(anchor.map(kSecond, host2), host2);
}

TEST(TimestampAnchor, SmallBackwardStepDoesNotReanchor) {
  TimestampAnchor anchor;
  anchor.map(10 * kSecond, kHost);
  EXPECT_EQ(anchor.map(10 * kSecond - kSecond / 2, kHost + 77 * kSecond), kHost - kSecond / 2);
}

}  // namespace
