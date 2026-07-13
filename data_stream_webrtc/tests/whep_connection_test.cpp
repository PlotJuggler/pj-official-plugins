// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// WhepConnection tests at the ACCESS-UNIT level (post-depacketizer), no RTP
// and no live PeerConnection: canned AUs go through the same normalize path
// the live onFrame uses (feedAccessUnitForTest / normalizeAccessUnit).
#include "whep_connection.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "video_emit.hpp"

namespace PJ {
namespace webrtc {
namespace {

const std::vector<uint8_t> kIdr = {0x65, 0x88};
const std::vector<uint8_t> kNonIdr = {0x41, 0x9A};

std::vector<uint8_t> avcc(const std::vector<std::vector<uint8_t>>& nals) {
  std::vector<uint8_t> au;
  for (const auto& n : nals) {
    const uint32_t len = static_cast<uint32_t>(n.size());
    au.push_back(static_cast<uint8_t>(len >> 24));
    au.push_back(static_cast<uint8_t>(len >> 16));
    au.push_back(static_cast<uint8_t>(len >> 8));
    au.push_back(static_cast<uint8_t>(len));
    au.insert(au.end(), n.begin(), n.end());
  }
  return au;
}

bool startsWithFourByteStartCode(const std::vector<uint8_t>& b) {
  return b.size() >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1;
}

TEST(WhepConnection, AvccIdrWithSdpSpropBecomesDecodableKeyframe) {
  auto au = avcc({kIdr});
  auto ef = WhepConnection::normalizeAccessUnit(au.data(), au.size(), /*ts_ns=*/1000, "Z0I=,aM4=");
  EXPECT_TRUE(ef.keyframe);
  EXPECT_EQ(ef.ts_ns, 1000);
  EXPECT_TRUE(startsWithFourByteStartCode(ef.annexb));
  EXPECT_TRUE(H264AnnexBNormalizer::containsIdr(ef.annexb));

  bool saw_sps = false;
  bool saw_pps = false;
  AnnexBIterator it(ef.annexb.data(), ef.annexb.size());
  for (NalView v = it.next(); v.data != nullptr; v = it.next()) {
    if (v.type() == kNalSps) {
      saw_sps = true;
    } else if (v.type() == kNalPps) {
      saw_pps = true;
    } else if (v.type() == kNalSliceIdr) {
      EXPECT_TRUE(saw_sps);
      EXPECT_TRUE(saw_pps);
      break;
    }
  }
}

TEST(WhepConnection, AnnexBKeyframePassthrough) {
  std::vector<uint8_t> annexb = {0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x00, 0x01,
                                 0x68, 0xCE, 0x00, 0x00, 0x01, 0x65, 0x88};
  auto ef = WhepConnection::normalizeAccessUnit(annexb.data(), annexb.size(), /*ts_ns=*/2000, "");
  EXPECT_TRUE(ef.keyframe);
  EXPECT_TRUE(startsWithFourByteStartCode(ef.annexb));
  EXPECT_TRUE(H264AnnexBNormalizer::containsIdr(ef.annexb));
}

TEST(WhepConnection, NonKeyframeIsNotMarkedKeyframe) {
  auto au = avcc({kNonIdr});
  auto ef = WhepConnection::normalizeAccessUnit(au.data(), au.size(), /*ts_ns=*/3000, "");
  EXPECT_FALSE(ef.keyframe);
  EXPECT_TRUE(startsWithFourByteStartCode(ef.annexb));
  EXPECT_FALSE(H264AnnexBNormalizer::containsIdr(ef.annexb));
}

TEST(WhepConnection, ExtractSpropFindsFirstVideoFmtp) {
  const std::string sdp =
      "v=0\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:video\r\n"
      "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z0I=,aM4=\r\n";
  EXPECT_EQ(WhepConnection::extractSpropForTest(sdp), "Z0I=,aM4=");
  EXPECT_EQ(WhepConnection::extractSpropForTest("v=0\r\nm=video 9\r\n"), "");
  // Video-scoped: a sprop outside any m=video section must be ignored.
  EXPECT_EQ(
      WhepConnection::extractSpropForTest("v=0\r\nm=audio 9 RTP/AVP 0\r\na=fmtp:0 sprop-parameter-sets=Z0I=,aM4=\r\n"),
      "");
}

TEST(WhepConnection, QueueIsBoundedDropOldest) {
  WhepConnection conn;  // nothing primed: non-IDR AUs pass through unchanged
  auto au = avcc({kNonIdr});
  for (int i = 0; i < 300; ++i) {
    conn.feedAccessUnitForTest(au.data(), au.size());
  }
  auto frames = conn.drain();
  EXPECT_EQ(frames.size(), 256u);  // kMaxQueuedFrames — oldest 44 dropped
}

TEST(WhepConnection, PrimedConnectionInjectsSpsBeforeIdrAndDrains) {
  WhepConnection conn;
  conn.primeFromAnswerForTest(
      "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "a=fmtp:96 sprop-parameter-sets=Z0I=,aM4=\r\n");
  auto au = avcc({kIdr});
  conn.feedAccessUnitForTest(au.data(), au.size());
  auto frames = conn.drain();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_TRUE(frames[0].keyframe);
  EXPECT_TRUE(H264AnnexBNormalizer::containsIdr(frames[0].annexb));
  // Swap-drain semantics: a second drain is empty.
  EXPECT_TRUE(conn.drain().empty());
}

TEST(WhepConnection, CloseIsIdempotentAndClearsQueue) {
  WhepConnection conn;
  auto au = avcc({kNonIdr});
  conn.feedAccessUnitForTest(au.data(), au.size());
  conn.close();
  EXPECT_TRUE(conn.drain().empty());
  EXPECT_EQ(conn.state(), ConnectionState::kClosed);
  conn.close();  // second close: no crash, still closed
  EXPECT_EQ(conn.state(), ConnectionState::kClosed);
}

TEST(WhepConnection, DifferentSpropsProduceDifferentParameterSets) {
  auto idr = avcc({kIdr});
  auto a = WhepConnection::normalizeAccessUnit(idr.data(), idr.size(), 1, "Z0I=,aM4=");
  auto b = WhepConnection::normalizeAccessUnit(idr.data(), idr.size(), 2, "Z01A,aO8=");
  ASSERT_TRUE(a.keyframe);
  ASSERT_TRUE(b.keyframe);
  EXPECT_NE(a.annexb, b.annexb);
}

}  // namespace
}  // namespace webrtc
}  // namespace PJ
