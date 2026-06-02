// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Receiver tests at the ACCESS-UNIT level (post-depacketizer), avoiding RTP and
// any live PeerConnection. libdatachannel's H264RtpDepacketizer emits one
// reassembled access unit per onFrame; this test feeds canned AUs into the
// same normalize path (WebrtcReceiver::normalizeAccessUnit) the live callback
// uses, and asserts the bytes satisfy the consumer contract.
#include "webrtc_receiver.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "video_emit.hpp"

namespace PJ {
namespace webrtc {
namespace {

const std::vector<uint8_t> kSps = {0x67, 0x42};
const std::vector<uint8_t> kPps = {0x68, 0xCE};
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

TEST(WebrtcReceiver, AvccIdrWithSdpSpropBecomesDecodableKeyframe) {
  // The depacketizer delivered a length-prefixed IDR; SPS/PPS came only from
  // the SDP (base64 SPS,PPS). The normalizer must inject them before the IDR.
  auto au = avcc({kIdr});
  auto ef = WebrtcReceiver::normalizeAccessUnit(au.data(), au.size(), /*ts_ns=*/1000, "Z0I=,aM4=");
  EXPECT_TRUE(ef.keyframe);
  EXPECT_EQ(ef.ts_ns, 1000);
  EXPECT_TRUE(startsWithFourByteStartCode(ef.annexb));
  EXPECT_TRUE(H264AnnexBNormalizer::containsIdr(ef.annexb));

  // SPS and PPS precede the IDR.
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

TEST(WebrtcReceiver, AnnexBKeyframePassthrough) {
  std::vector<uint8_t> annexb = {0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x00, 0x01,
                                 0x68, 0xCE, 0x00, 0x00, 0x01, 0x65, 0x88};
  auto ef = WebrtcReceiver::normalizeAccessUnit(annexb.data(), annexb.size(), /*ts_ns=*/2000, "");
  EXPECT_TRUE(ef.keyframe);
  EXPECT_TRUE(startsWithFourByteStartCode(ef.annexb));
  EXPECT_TRUE(H264AnnexBNormalizer::containsIdr(ef.annexb));
}

TEST(WebrtcReceiver, NonKeyframeIsNotMarkedKeyframe) {
  auto au = avcc({kNonIdr});
  auto ef = WebrtcReceiver::normalizeAccessUnit(au.data(), au.size(), /*ts_ns=*/3000, "");
  EXPECT_FALSE(ef.keyframe);
  EXPECT_TRUE(startsWithFourByteStartCode(ef.annexb));
  EXPECT_FALSE(H264AnnexBNormalizer::containsIdr(ef.annexb));
}

TEST(WebrtcReceiver, PerMidNormalizersDoNotCrossContaminate) {
  // Two cameras with DIFFERENT SPS/PPS. Each normalized via its own sprop must
  // inject ITS OWN parameter sets — proving one-normalizer-per-mid is required.
  const std::string spropA = "Z0I=,aM4=";  // SPS {0x67,0x42}, PPS {0x68,0xCE}
  const std::string spropB = "Z01A,aO8=";  // SPS {0x67,0x4D,0x40}, PPS {0x68,0xEF}

  auto idr = avcc({kIdr});
  auto a = WebrtcReceiver::normalizeAccessUnit(idr.data(), idr.size(), 1, spropA);
  auto b = WebrtcReceiver::normalizeAccessUnit(idr.data(), idr.size(), 2, spropB);

  ASSERT_TRUE(a.keyframe);
  ASSERT_TRUE(b.keyframe);
  // The injected SPS/PPS bytes differ between the two cameras.
  EXPECT_NE(a.annexb, b.annexb);
  EXPECT_TRUE(H264AnnexBNormalizer::containsIdr(a.annexb));
  EXPECT_TRUE(H264AnnexBNormalizer::containsIdr(b.annexb));
}

}  // namespace
}  // namespace webrtc
}  // namespace PJ
