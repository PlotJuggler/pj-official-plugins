#include "../blf_frames.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using blf_detail::CanFrame;

// Reads the committed sample.blf (3 known frames on 2 channels, generated with
// python-can — see test_data/gen_blf.py) and checks the decoded frame tuple.
TEST(BlfFrames, ReadsBundledFixture) {
  const std::string path = (std::filesystem::path(BLF_TEST_DATA_DIR) / "sample.blf").string();
  std::vector<CanFrame> frames;
  blf_detail::BlfStats stats;
  const auto status = blf_detail::readCanFrames(path, [&](const CanFrame& f) { frames.push_back(f); }, stats);
  ASSERT_TRUE(status.has_value()) << status.error();
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(stats.can_frames, 3u);

  // Frame 0: channel 1, standard id 0x100, data E8 03 B8 0B 00...
  EXPECT_EQ(frames[0].channel, 1u);
  EXPECT_EQ(frames[0].can_id, 0x100u);
  EXPECT_FALSE(frames[0].extended);
  ASSERT_EQ(frames[0].data.size(), 8u);
  EXPECT_EQ(frames[0].data[0], 0xE8);
  EXPECT_EQ(frames[0].data[1], 0x03);

  // Frame 2: channel 2, extended (29-bit J1939) id 0x18FEF100, data 01..08.
  EXPECT_EQ(frames[2].channel, 2u);
  EXPECT_EQ(frames[2].can_id, 0x18FEF100u);
  EXPECT_TRUE(frames[2].extended);
  EXPECT_EQ(frames[2].data[0], 0x01);

  // Relative timestamps are deterministic: +10 ms and +20 ms from the first.
  EXPECT_EQ(frames[1].ts_ns - frames[0].ts_ns, 10'000'000);
  EXPECT_EQ(frames[2].ts_ns - frames[0].ts_ns, 20'000'000);
}

}  // namespace
