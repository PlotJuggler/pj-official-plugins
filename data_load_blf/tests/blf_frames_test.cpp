#include "../blf_frames.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <pj_can_dbc/can_decoder.hpp>
#include <string>
#include <vector>

namespace {

using blf_detail::CanFrame;

std::vector<CanFrame> readFixtureFrames() {
  const std::string path = (std::filesystem::path(BLF_TEST_DATA_DIR) / "sample.blf").string();
  std::vector<CanFrame> frames;
  blf_detail::BlfStats stats;
  (void)blf_detail::readCanFrames(
      path,
      [&](const CanFrame& f) {
        frames.push_back(f);
        return true;
      },
      stats);
  return frames;
}

// Reads the committed sample.blf (3 known frames on 2 channels, generated with
// python-can — see test_data/gen_blf.py) and checks the decoded frame tuple.
TEST(BlfFrames, ReadsBundledFixture) {
  const std::string path = (std::filesystem::path(BLF_TEST_DATA_DIR) / "sample.blf").string();
  std::vector<CanFrame> frames;
  blf_detail::BlfStats stats;
  const auto status = blf_detail::readCanFrames(
      path,
      [&](const CanFrame& f) {
        frames.push_back(f);
        return true;
      },
      stats);
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

// End-to-end: BLF frames -> CanDecoder (the core of importData). The fixture's
// channel-1 frames carry id 0x100 with Speed at bits 0..15 (raw 0x03E8 = 1000,
// scaled x0.1 -> 100.0 km/h).
TEST(BlfDecode, FixtureFramesDecodeThroughDbc) {
  const char* const kDbc = R"DBC(VERSION "1.0.0"

NS_ :

BS_:

BU_: ECU

BO_ 256 EngineData: 8 ECU
 SG_ Speed : 0|16@1+ (0.1,0) [0|6553.5] "km/h" ECU
)DBC";

  mf4_detail::CanDecoder decoder;
  ASSERT_TRUE(decoder.loadDbcString(kDbc).has_value());

  const auto frames = readFixtureFrames();
  ASSERT_EQ(frames.size(), 3u);

  bool matched = false;
  const auto signals = decoder.decode(frames[0].can_id, frames[0].extended, frames[0].data, matched);
  EXPECT_TRUE(matched);
  ASSERT_FALSE(signals.empty());
  EXPECT_EQ(signals[0].name, "Speed");
  EXPECT_DOUBLE_EQ(signals[0].value, 100.0);
  EXPECT_EQ(signals[0].unit, "km/h");
}

}  // namespace
