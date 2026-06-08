// Unit tests for pj_video_demux. The byte-rewrite core (avccToAnnexB,
// prependAv1SeqHeader) is fixture-free; per-codec index+read e2e tests run
// against the committed h264/hevc/av1 fixtures in tests/data/ (path injected via
// PJ_VIDEO_DEMUX_TEST_DATA_DIR). An optional PJ_TEST_VIDEO env var points the
// e2e at an external file instead.
#include "pj_video_demux/video_demux.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using PJ::Span;
using PJ::video_demux::avccToAnnexB;
using PJ::video_demux::prependAv1SeqHeader;

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

TEST(VideoDemuxTest, Av1PrependsSeqHeaderOnKeyframe) {
  // AV1 OBUs have no start codes; the seq-header configOBUs are prepended raw.
  const std::vector<uint8_t> seq = {0x0A, 0x0B, 0x00};     // (pretend) sequence header OBU
  const std::vector<uint8_t> sample = {0x32, 0x01, 0xFF};  // a frame OBU
  const auto out = prependAv1SeqHeader(span(sample), span(seq), /*keyframe=*/true);
  const std::vector<uint8_t> expected = {0x0A, 0x0B, 0x00, 0x32, 0x01, 0xFF};
  EXPECT_EQ(out, expected);
}

TEST(VideoDemuxTest, Av1PassesNonKeyframeThrough) {
  const std::vector<uint8_t> seq = {0x0A, 0x0B, 0x00};
  const std::vector<uint8_t> sample = {0x32, 0x01, 0xFF};
  const auto out = prependAv1SeqHeader(span(sample), span(seq), /*keyframe=*/false);
  EXPECT_EQ(out, sample);
}

// Index a committed fixture, then read its first access unit. The first AU must
// be a keyframe, and (since keyframes carry the parameter sets / seq header) the
// assembled bytes must begin with `param_sets` — the codec-agnostic proxy for a
// self-decodable keyframe.
void indexAndReadFixture(const std::string& path, const std::string& expected_format) {
  auto idx = PJ::video_demux::indexFile(path);
  ASSERT_TRUE(idx.has_value()) << idx.error();
  EXPECT_EQ(idx->format, expected_format);
  ASSERT_FALSE(idx->units.empty());
  EXPECT_TRUE(idx->units.front().keyframe) << "the first access unit should be a keyframe";
  ASSERT_FALSE(idx->param_sets.empty()) << "a keyframe-decodable index must carry parameter sets";

  auto reader = PJ::video_demux::LazyAccessUnitReader::create(path, idx->format, idx->param_sets, idx->nal_length_size);
  auto bytes = reader->readUnit(idx->units.front());
  ASSERT_TRUE(bytes.has_value()) << bytes.error();
  ASSERT_GE(bytes->size(), idx->param_sets.size());
  EXPECT_TRUE(std::equal(idx->param_sets.begin(), idx->param_sets.end(), bytes->begin()))
      << "keyframe must begin with the parameter sets (" << expected_format << ")";
}

std::string fixture(const char* name) {
#ifdef PJ_VIDEO_DEMUX_TEST_DATA_DIR
  return std::string(PJ_VIDEO_DEMUX_TEST_DATA_DIR) + "/" + name;
#else
  return name;
#endif
}

TEST(VideoDemuxTest, IndexAndReadH264Fixture) {
  indexAndReadFixture(fixture("test_h264.mp4"), "h264");
}

TEST(VideoDemuxTest, IndexAndReadHevcFixture) {
  indexAndReadFixture(fixture("test_hevc.mp4"), "h265");
}

TEST(VideoDemuxTest, IndexAndReadAv1Fixture) {
  indexAndReadFixture(fixture("test_av1.mp4"), "av1");
}

TEST(VideoDemuxTest, IndexAndReadExternalVideo) {
  const char* path = std::getenv("PJ_TEST_VIDEO");
  if (path == nullptr) {
    GTEST_SKIP() << "set PJ_TEST_VIDEO to an h264/h265/av1 .mp4 to run the external index+read test";
  }
  auto idx = PJ::video_demux::indexFile(path);
  ASSERT_TRUE(idx.has_value()) << idx.error();
  ASSERT_FALSE(idx->units.empty());
  EXPECT_TRUE(idx->units.front().keyframe) << "the first access unit should be a keyframe";
  auto reader = PJ::video_demux::LazyAccessUnitReader::create(path, idx->format, idx->param_sets, idx->nal_length_size);
  auto bytes = reader->readUnit(idx->units.front());
  ASSERT_TRUE(bytes.has_value()) << bytes.error();
  EXPECT_FALSE(bytes->empty());
}

}  // namespace
