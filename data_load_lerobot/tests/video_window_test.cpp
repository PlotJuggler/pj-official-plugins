// Unit tests for lerobot::resolveEmitSlice — the v2.x-vs-v3.0 video window logic
// (keyframe-seek-back, presentation window, episode-local rebase). Pure: drives
// synthetic AccessUnit indices, no host and no libav.
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "lerobot_video_window.hpp"

namespace {

using PJ::video_demux::AccessUnit;

// Build a decode-order unit. For these tests only dts/pts/keyframe matter.
AccessUnit unit(int64_t dts_ns, int64_t pts_ns, bool keyframe) {
  AccessUnit au;
  au.dts_ns = dts_ns;
  au.pts_ns = pts_ns;
  au.keyframe = keyframe;
  au.size = 1;
  return au;
}

// No-B-frame stream: dts == pts, keyframe at GOP starts (every 3rd here).
std::vector<AccessUnit> simpleStream() {
  return {
      unit(0, 0, true),   unit(10, 10, false), unit(20, 20, false),
      unit(30, 30, true), unit(40, 40, false), unit(50, 50, false),
  };
}

TEST(ResolveEmitSlice, NoWindowEmitsWholeFileRebasedToFirstDts) {
  const auto units = simpleStream();
  const auto slice = lerobot::resolveEmitSlice(units, std::nullopt, std::nullopt);
  EXPECT_EQ(slice.first_idx, 0u);
  EXPECT_EQ(slice.last_idx, units.size() - 1);
  EXPECT_EQ(slice.origin_ns, 0);
}

TEST(ResolveEmitSlice, NoWindowOriginIsFirstDtsEvenWhenNegative) {
  // Encoder-delay convention: first DTS negative. Origin must follow so the
  // first emitted frame lands on episode-local 0.
  const std::vector<AccessUnit> units = {unit(-2'000, -2'000, true), unit(-1'000, -1'000, false), unit(0, 0, false)};
  const auto slice = lerobot::resolveEmitSlice(units, std::nullopt, std::nullopt);
  EXPECT_EQ(slice.first_idx, 0u);
  EXPECT_EQ(slice.last_idx, 2u);
  EXPECT_EQ(slice.origin_ns, -2'000);
}

TEST(ResolveEmitSlice, WindowSeeksBackToKeyframeAtOrBeforeStart) {
  const auto units = simpleStream();
  // Window starts at 35 (inside the second GOP, whose keyframe is unit 3 @30).
  const auto slice = lerobot::resolveEmitSlice(units, std::optional<int64_t>(35), std::optional<int64_t>(100));
  EXPECT_EQ(slice.first_idx, 3u) << "must seek back to the keyframe at/ before start";
  EXPECT_EQ(slice.origin_ns, 35) << "origin is the window start, not the keyframe pts";
  EXPECT_EQ(slice.last_idx, units.size() - 1);
}

TEST(ResolveEmitSlice, WindowStartBeforeFirstKeyframeFallsBackToZero) {
  const auto units = simpleStream();
  const auto slice = lerobot::resolveEmitSlice(units, std::optional<int64_t>(-100), std::optional<int64_t>(25));
  EXPECT_EQ(slice.first_idx, 0u);
  EXPECT_EQ(slice.origin_ns, -100);
}

TEST(ResolveEmitSlice, WindowEndExcludesLaterFrames) {
  const auto units = simpleStream();
  // [0, 35): last unit presented before 35 is unit 3 (pts 30).
  const auto slice = lerobot::resolveEmitSlice(units, std::optional<int64_t>(0), std::optional<int64_t>(35));
  EXPECT_EQ(slice.first_idx, 0u);
  EXPECT_EQ(slice.last_idx, 3u);
  EXPECT_EQ(slice.origin_ns, 0);
}

TEST(ResolveEmitSlice, OpenEndedWindowRunsToEnd) {
  const auto units = simpleStream();
  const auto slice = lerobot::resolveEmitSlice(units, std::optional<int64_t>(0), std::nullopt);
  EXPECT_EQ(slice.last_idx, units.size() - 1);
}

TEST(ResolveEmitSlice, BFrameReorderStaysContiguousThroughWindowEnd) {
  // Decode order with B-frame reordering: dts monotonic, pts non-monotonic.
  const std::vector<AccessUnit> units = {
      unit(0, 0, true),    unit(10, 30, false), unit(20, 10, false), unit(30, 20, false),
      unit(40, 60, false), unit(50, 40, false), unit(60, 50, false),
  };
  // End at 45: the last unit with pts < 45 is index 5 (pts 40). Index 4 (pts 60)
  // sits inside [first, last] and must stay for decode continuity.
  const auto slice = lerobot::resolveEmitSlice(units, std::optional<int64_t>(0), std::optional<int64_t>(45));
  EXPECT_EQ(slice.first_idx, 0u);
  EXPECT_EQ(slice.last_idx, 5u) << "contiguous through the last in-window pts, keeping reordered frames";
}

TEST(ResolveEmitSlice, EmptyUnitsIsSafe) {
  const std::vector<AccessUnit> units;
  const auto slice = lerobot::resolveEmitSlice(units, std::nullopt, std::nullopt);
  EXPECT_EQ(slice.first_idx, 0u);
  EXPECT_EQ(slice.last_idx, 0u);
  EXPECT_EQ(slice.origin_ns, 0);
}

}  // namespace
