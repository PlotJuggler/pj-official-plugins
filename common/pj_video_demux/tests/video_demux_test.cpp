// Unit tests for pj_video_demux. The AVCC→Annex-B core is fixture-free; an
// end-to-end index+read test runs only when PJ_TEST_VIDEO points at an H.264
// .mp4 (it is GTEST_SKIPped otherwise, mirroring the host video tests).
#include "pj_video_demux/video_demux.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

using PJ::Span;
using PJ::video_demux::avccToAnnexB;

Span<const uint8_t> span(const std::vector<uint8_t>& v) {
  return Span<const uint8_t>(v.data(), v.size());
}

TEST(VideoDemuxTest, AvccToAnnexBRewritesLengthPrefixedNals) {
  // Two AVCC NALs (4-byte big-endian length prefixes): {0x41,0xAA} then {0x01}.
  const std::vector<uint8_t> avcc = {0x00, 0x00, 0x00, 0x02, 0x41, 0xAA, 0x00, 0x00, 0x00, 0x01, 0x01};
  const std::vector<uint8_t> params = {0x00, 0x00, 0x00, 0x01, 0x67, 0x00, 0x00, 0x00, 0x01, 0x68};

  // Non-keyframe: parameter sets are NOT prepended.
  const auto out = avccToAnnexB(span(avcc), span(params), /*keyframe=*/false, /*nal_length_size=*/4);
  const std::vector<uint8_t> expected = {0x00, 0x00, 0x00, 0x01, 0x41, 0xAA, 0x00, 0x00, 0x00, 0x01, 0x01};
  EXPECT_EQ(out, expected);
}

TEST(VideoDemuxTest, AvccToAnnexBPrependsParamsOnKeyframe) {
  const std::vector<uint8_t> avcc = {0x00, 0x00, 0x00, 0x01, 0x65};  // IDR slice (NAL type 5)
  const std::vector<uint8_t> params = {0x00, 0x00, 0x00, 0x01, 0x67, 0x00, 0x00, 0x00, 0x01, 0x68};

  const auto out = avccToAnnexB(span(avcc), span(params), /*keyframe=*/true, 4);
  const std::vector<uint8_t> expected = {0x00, 0x00, 0x00, 0x01, 0x67,   // SPS
                                         0x00, 0x00, 0x00, 0x01, 0x68,   // PPS
                                         0x00, 0x00, 0x00, 0x01, 0x65};  // IDR
  EXPECT_EQ(out, expected);
}

TEST(VideoDemuxTest, AvccToAnnexBStopsAtTruncatedNal) {
  // Length prefix claims 5 bytes but only 2 follow → emit nothing, clean stop.
  const std::vector<uint8_t> avcc = {0x00, 0x00, 0x00, 0x05, 0x41, 0xAA};
  const auto out = avccToAnnexB(span(avcc), Span<const uint8_t>(), /*keyframe=*/false, 4);
  EXPECT_TRUE(out.empty());
}

TEST(VideoDemuxTest, IndexAndReadRealMp4) {
  const char* path = std::getenv("PJ_TEST_VIDEO");
  if (path == nullptr) {
    GTEST_SKIP() << "set PJ_TEST_VIDEO to an H.264 .mp4 to run the end-to-end index+read test";
  }
  auto idx = PJ::video_demux::indexFile(path);
  ASSERT_TRUE(idx.has_value()) << idx.error();
  EXPECT_EQ(idx->format, "h264");
  ASSERT_FALSE(idx->units.empty());
  EXPECT_TRUE(idx->units.front().keyframe) << "the first access unit should be a keyframe";

  auto reader = PJ::video_demux::LazyAnnexBReader::create(path, idx->annexb_params, idx->nal_length_size);
  auto bytes = reader->readUnit(idx->units.front());
  ASSERT_TRUE(bytes.has_value()) << bytes.error();
  // Annex-B: starts with a 4-byte start code; the keyframe carries SPS+PPS.
  ASSERT_GE(bytes->size(), 5u);
  EXPECT_EQ((*bytes)[0], 0x00);
  EXPECT_EQ((*bytes)[1], 0x00);
  EXPECT_EQ((*bytes)[2], 0x00);
  EXPECT_EQ((*bytes)[3], 0x01);
}

}  // namespace
