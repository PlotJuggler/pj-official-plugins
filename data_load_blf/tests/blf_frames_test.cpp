#include "../blf_frames.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
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
  // The BLF header's object count feeds import progress; live counts
  // (can_frames + skipped_objects) advance against it during the read.
  EXPECT_GE(stats.total_objects, 3u);

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

  pj_can_dbc::CanDecoder decoder;
  ASSERT_TRUE(decoder.loadDbcString(kDbc).has_value());

  const auto frames = readFixtureFrames();
  ASSERT_EQ(frames.size(), 3u);

  pj_can_dbc::DecodeResult result = pj_can_dbc::DecodeResult::kNoMatch;
  const auto signals = decoder.decode(frames[0].can_id, frames[0].extended, frames[0].data, result);
  EXPECT_EQ(result, pj_can_dbc::DecodeResult::kDecoded);
  ASSERT_FALSE(signals.empty());
  EXPECT_EQ(signals[0].name, "Speed");
  EXPECT_DOUBLE_EQ(signals[0].value, 100.0);
  EXPECT_EQ(signals[0].unit, "km/h");
}

}  // namespace

TEST(ParseChannelKey, AcceptsPlainDecimalChannels) {
  EXPECT_EQ(blf_detail::parseChannelKey("1"), std::uint16_t{1});
  EXPECT_EQ(blf_detail::parseChannelKey("42"), std::uint16_t{42});
  EXPECT_EQ(blf_detail::parseChannelKey("65535"), std::uint16_t{65535});
}

TEST(ParseChannelKey, RejectsNonNumericAndOutOfRangeKeys) {
  // Saved configs are hand-editable (the README directs users to edit
  // channel_dbcs for channels above 4) — a typo must not throw or wrap.
  EXPECT_EQ(blf_detail::parseChannelKey(""), std::nullopt);
  EXPECT_EQ(blf_detail::parseChannelKey("ch1"), std::nullopt);
  EXPECT_EQ(blf_detail::parseChannelKey("1x"), std::nullopt);
  EXPECT_EQ(blf_detail::parseChannelKey("-1"), std::nullopt);
  EXPECT_EQ(blf_detail::parseChannelKey("65536"), std::nullopt);  // would wrap to 0
  EXPECT_EQ(blf_detail::parseChannelKey("999999999999"), std::nullopt);
}

// BLF object timestamps are file-controlled 64-bit tick counts; the 10-us
// flag multiplies by 10000, so hostile values must saturate, not overflow
// (and a huge unsigned tick count must not wrap negative).
TEST(BlfTime, ObjectTimeSaturatesInsteadOfOverflowing) {
  EXPECT_EQ(blf_detail::objectTimeNs(0x1u, 42), 420'000);
  EXPECT_EQ(blf_detail::objectTimeNs(0x2u, 42), 42);
  EXPECT_EQ(
      blf_detail::objectTimeNs(0x1u, std::numeric_limits<std::uint64_t>::max()),
      std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(
      blf_detail::objectTimeNs(0x2u, std::numeric_limits<std::uint64_t>::max()),
      std::numeric_limits<std::int64_t>::max());
}

// Writes sample.blf with the first LOG_CONTAINER's objSize (BaseHeader offset
// 8) overwritten, and reads it back through readCanFrames.
namespace {
PJ::Status readWithPokedObjSize(std::uint32_t obj_size, const std::string& out_name) {
  const std::string src = (std::filesystem::path(BLF_TEST_DATA_DIR) / "sample.blf").string();
  std::ifstream in(src, std::ios::binary);
  EXPECT_TRUE(in.is_open());
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  std::uint32_t stat_size = 0;
  EXPECT_GE(bytes.size(), 8u);
  std::memcpy(&stat_size, bytes.data() + 4, sizeof(stat_size));
  EXPECT_LT(static_cast<std::size_t>(stat_size) + 16, bytes.size());
  std::memcpy(bytes.data() + stat_size + 8, &obj_size, sizeof(obj_size));
  const auto out = (std::filesystem::temp_directory_path() / out_name).string();
  {
    std::ofstream os(out, std::ios::binary);
    os.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  blf_detail::BlfStats stats;
  return blf_detail::readCanFrames(out, [](const blf_detail::CanFrame&) { return true; }, stats);
}
}  // namespace

// A LOG_CONTAINER whose objSize is smaller than its own header underflows the
// blob-size math in the pinned lblf, requesting a multi-GB allocation from a
// tiny file. Our vendored guard must turn that into a clean read error.
TEST(BlfFrames, UndersizedLogContainerFailsCleanly) {
  const auto status = readWithPokedObjSize(20, "blf_hostile_container_small.blf");
  ASSERT_FALSE(status.has_value()) << "undersized log container must fail, not allocate";
  // The guard must reject the container up front (rather than underflowing into
  // a multi-GB allocation that only incidentally errors out later).
  EXPECT_NE(status.error().find("log container"), std::string::npos) << "actual: " << status.error();
}

// A forged large objSize (exceeding the file) must also be rejected up front —
// otherwise lblf resize()s a buffer to ~objSize bytes before the short read
// surfaces (an allocation DoS from a small file).
TEST(BlfFrames, OversizedLogContainerFailsCleanly) {
  const auto status = readWithPokedObjSize(0xF0000000u, "blf_hostile_container_big.blf");
  ASSERT_FALSE(status.has_value()) << "oversized log container must fail, not allocate";
  EXPECT_NE(status.error().find("log container"), std::string::npos) << "actual: " << status.error();
}
