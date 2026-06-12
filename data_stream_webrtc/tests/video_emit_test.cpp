// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Byte-compatibility gate for the WebRTC -> PJ.VideoFrame producer path.
// We REPLICATE the consumer's isH264Keyframe / extractH264SpsPps NAL scan here
// (the app's h264_utils is not linkable from a plugin) to prove the emitted
// bytes pass the real validator.
#include "video_emit.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <pj_base/builtin/video_frame_codec.hpp>
#include <vector>

namespace PJ {
namespace webrtc {
namespace {

// Minimal NAL fixtures: header byte + 1 payload byte.
const std::vector<uint8_t> kSps = {0x67, 0x42};     // type 7
const std::vector<uint8_t> kPps = {0x68, 0xCE};     // type 8
const std::vector<uint8_t> kIdr = {0x65, 0x88};     // type 5 (IDR)
const std::vector<uint8_t> kNonIdr = {0x41, 0x9A};  // type 1 (non-IDR)

// Build an AVCC-style (4-byte big-endian length) access unit from raw NALs.
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

// --- Replica of the consumer's NAL scanning (h264_utils.cpp) ---
size_t findStartCode(const uint8_t* d, size_t n, size_t off) {
  while (off + 2 < n) {
    if (d[off] == 0 && d[off + 1] == 0) {
      if (d[off + 2] == 1) {
        return off;
      }
      if (off + 3 < n && d[off + 2] == 0 && d[off + 3] == 1) {
        return off;
      }
    }
    ++off;
  }
  return n;
}
size_t nalHeaderOffset(const uint8_t* d, size_t off) {
  return d[off + 2] == 1 ? off + 3 : off + 4;
}

bool consumerIsKeyframe(const std::vector<uint8_t>& b) {
  const uint8_t* d = b.data();
  const size_t n = b.size();
  if (n < 4) {
    return false;
  }
  for (size_t p = findStartCode(d, n, 0); p < n;) {
    const size_t h = nalHeaderOffset(d, p);
    if (h < n && (d[h] & 0x1F) == 5) {
      return true;
    }
    p = findStartCode(d, n, h);
  }
  return false;
}

bool consumerHasSpsAndPpsBeforeIdr(const std::vector<uint8_t>& b) {
  const uint8_t* d = b.data();
  const size_t n = b.size();
  bool sps = false;
  bool pps = false;
  for (size_t p = findStartCode(d, n, 0); p < n;) {
    const size_t h = nalHeaderOffset(d, p);
    if (h >= n) {
      break;
    }
    const uint8_t t = static_cast<uint8_t>(d[h] & 0x1F);
    if (t == 7) {
      sps = true;
    }
    if (t == 8) {
      pps = true;
    }
    if (t == 5 || t == 1) {
      break;  // consumer stops at the first slice
    }
    p = findStartCode(d, n, h);
  }
  return sps && pps;
}

TEST(VideoEmit, EmitsFourByteStartCodes) {
  H264AnnexBNormalizer norm;
  auto au = avcc({kNonIdr});
  bool kf = true;
  auto out = norm.normalize(au.data(), au.size(), kf);
  ASSERT_GE(out.size(), 4u);
  EXPECT_EQ(out[0], 0x00);
  EXPECT_EQ(out[1], 0x00);
  EXPECT_EQ(out[2], 0x00);
  EXPECT_EQ(out[3], 0x01);
  EXPECT_FALSE(kf);
}

TEST(VideoEmit, KeyframeCarriesSpsPpsFromSprop) {
  H264AnnexBNormalizer norm;
  norm.prime(kSps, kPps);
  auto au = avcc({kIdr});  // IDR only; params arrive out-of-band
  bool kf = false;
  auto out = norm.normalize(au.data(), au.size(), kf);
  EXPECT_TRUE(kf);
  EXPECT_TRUE(consumerIsKeyframe(out));
  EXPECT_TRUE(consumerHasSpsAndPpsBeforeIdr(out)) << "SPS+PPS must precede the IDR or extractH264SpsPps returns empty";
}

TEST(VideoEmit, InBandSpsPpsNotDuplicated) {
  H264AnnexBNormalizer norm;
  norm.prime(kSps, kPps);
  auto au = avcc({kSps, kPps, kIdr});
  bool kf = false;
  auto out = norm.normalize(au.data(), au.size(), kf);
  EXPECT_TRUE(consumerIsKeyframe(out));
  EXPECT_TRUE(consumerHasSpsAndPpsBeforeIdr(out));
  size_t sps_count = 0;
  AnnexBIterator it(out.data(), out.size());
  for (NalView v = it.next(); v.data != nullptr; v = it.next()) {
    if (v.type() == kNalSps) {
      ++sps_count;
    }
  }
  EXPECT_EQ(sps_count, 1u);
}

TEST(VideoEmit, AnnexBInputPassthrough) {
  std::vector<uint8_t> annexb = {0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x00, 0x01,
                                 0x68, 0xCE, 0x00, 0x00, 0x01, 0x65, 0x88};
  H264AnnexBNormalizer norm;
  bool kf = false;
  auto out = norm.normalize(annexb.data(), annexb.size(), kf);
  EXPECT_TRUE(kf);
  EXPECT_TRUE(consumerIsKeyframe(out));
  EXPECT_TRUE(consumerHasSpsAndPpsBeforeIdr(out));
}

TEST(VideoEmit, SpropDecodesBase64ParameterSets) {
  // base64(0x67 0x42) = "Z0I=", base64(0x68 0xCE) = "aM4=".
  auto nals = parseSpropParameterSets("Z0I=,aM4=");
  ASSERT_EQ(nals.size(), 2u);
  EXPECT_EQ(nalType(nals[0][0]), kNalSps);
  EXPECT_EQ(nalType(nals[1][0]), kNalPps);
}

// Stub runtime host: capture the pushed payload by invoking the fetcher.
TEST(VideoEmit, PushVideoFrameSerializesValidFrame) {
  H264AnnexBNormalizer norm;
  norm.prime(kSps, kPps);
  auto au = avcc({kIdr});
  EncodedFrame ef;
  ef.ts_ns = 123456789;
  ef.annexb = norm.normalize(au.data(), au.size(), ef.keyframe);

  // Serialize directly (the same call pushVideoFrame makes) and round-trip to
  // assert the wire frame is well-formed PJ.VideoFrame with format "h264".
  sdk::VideoFrame vf;
  vf.timestamp_ns = ef.ts_ns;
  vf.frame_id = "webrtc";
  vf.format = "h264";
  vf.data = PJ::Span<const uint8_t>(ef.annexb.data(), ef.annexb.size());
  auto wire = serializeVideoFrame(vf);
  ASSERT_FALSE(wire.empty());

  auto decoded = deserializeVideoFrame(wire.data(), wire.size());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->format, "h264");
  EXPECT_EQ(decoded->timestamp_ns, ef.ts_ns);
  ASSERT_EQ(decoded->data.size(), ef.annexb.size());
  std::vector<uint8_t> rt(decoded->data.begin(), decoded->data.end());
  EXPECT_TRUE(consumerIsKeyframe(rt));
  EXPECT_TRUE(consumerHasSpsAndPpsBeforeIdr(rt));
}

}  // namespace
}  // namespace webrtc
}  // namespace PJ
