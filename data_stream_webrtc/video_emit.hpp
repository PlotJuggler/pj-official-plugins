// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Qt-free, FFmpeg-free, libdatachannel-free consumer-conformance core for the
// data_stream_webrtc plugin. Everything here is unit-testable headlessly.
//
// Consumer contract this satisfies (pj_scene2D/core h264_utils.cpp +
// streaming_video_decoder.cpp):
//   * Annex-B byte stream (this emitter writes 4-byte start codes 00 00 00 01,
//     the repo convention asserted by video_mcap_roundtrip_test.cpp).
//   * One access unit per VideoFrame.data.
//   * isH264Keyframe(): an IDR slice (NAL type 5) is present.
//   * extractH264SpsPps(): SPS(7)+PPS(8) MUST physically precede the IDR slice
//     in the same buffer, or the decoder gets empty extradata.
//   * No B-frames (encoder is configured low-latency upstream).
#pragma once

#include <cstddef>
#include <cstdint>
#include <pj_base/buffer_anchor.hpp>
#include <pj_base/expected.hpp>
#include <pj_base/sdk/data_source_host_views.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace PJ {
namespace webrtc {

// H.264 NAL unit types (header byte & 0x1F). Mirrors the consumer's constants;
// the app's h264_utils helpers are app-side and not linkable from a plugin.
enum NalType : uint8_t {
  kNalSliceNonIdr = 1,
  kNalSliceIdr = 5,
  kNalSps = 7,
  kNalPps = 8,
};

inline uint8_t nalType(uint8_t header_byte) {
  return static_cast<uint8_t>(header_byte & 0x1F);
}

// One reassembled access unit handed up from the receiver, already Annex-B
// normalized and ready to serialize. `ts_ns` is the host wall-clock timestamp.
struct EncodedFrame {
  int64_t ts_ns = 0;
  bool keyframe = false;
  std::vector<uint8_t> annexb;
};

// A view over one NAL unit's payload (header byte is data[0]).
struct NalView {
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint8_t type() const {
    return size > 0 ? nalType(data[0]) : static_cast<uint8_t>(0xFF);
  }
};

// Iterates Annex-B NAL units, handling both 3-byte (00 00 01) and 4-byte
// (00 00 00 01) start codes exactly like the consumer's findStartCode.
class AnnexBIterator {
 public:
  AnnexBIterator(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  NalView next() {
    size_t sc = findStartCode(pos_);
    if (sc >= size_) {
      return {};
    }
    size_t hdr = nalHeaderOffset(sc);
    if (hdr >= size_) {
      return {};
    }
    size_t next_sc = findStartCode(hdr);
    pos_ = next_sc;
    return NalView{data_ + hdr, next_sc - hdr};
  }

 private:
  size_t findStartCode(size_t offset) const {
    while (offset + 2 < size_) {
      if (data_[offset] == 0x00 && data_[offset + 1] == 0x00) {
        if (data_[offset + 2] == 0x01) {
          return offset;
        }
        if (offset + 3 < size_ && data_[offset + 2] == 0x00 && data_[offset + 3] == 0x01) {
          return offset;
        }
      }
      ++offset;
    }
    return size_;
  }
  size_t nalHeaderOffset(size_t sc) const {
    return data_[sc + 2] == 0x01 ? sc + 3 : sc + 4;  // 3- vs 4-byte start code
  }
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

// Normalizes one access unit (post-depacketizer) into a 4-byte-start-code
// Annex-B buffer and guarantees SPS+PPS sit immediately before the IDR on
// keyframes. Accepts either Annex-B input (already framed) or AVCC-style
// length-prefixed input (libdatachannel's default depacketizer separator).
class H264AnnexBNormalizer {
 public:
  // Prime with SPS/PPS parsed from the SDP sprop-parameter-sets (raw NAL bytes,
  // NO start code, header byte first). Optional; empty disables injection.
  void prime(std::vector<uint8_t> sps, std::vector<uint8_t> pps) {
    primed_sps_ = std::move(sps);
    primed_pps_ = std::move(pps);
  }

  bool hasPrimedParameterSets() const {
    return !primed_sps_.empty() && !primed_pps_.empty();
  }

  // Returns true if the produced access unit contains an IDR slice.
  static bool containsIdr(const std::vector<uint8_t>& annexb);

  // Input = one access unit from the depacketizer. Output = Annex-B access
  // unit with 4-byte start codes; on a keyframe, primed SPS/PPS are injected
  // before the IDR when not already in-band. Sets out_keyframe.
  std::vector<uint8_t> normalize(const uint8_t* au, size_t size, bool& out_keyframe) const;

 private:
  struct NalSpan {
    const uint8_t* data;
    size_t size;
  };

  static bool looksAnnexB(const uint8_t* d, size_t n) {
    if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) {
      return true;
    }
    if (n >= 3 && d[0] == 0 && d[1] == 0 && d[2] == 1) {
      return true;
    }
    return false;
  }
  static void appendNal(std::vector<uint8_t>& out, const uint8_t* nal, size_t len) {
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});  // 4-byte start code: repo convention
    out.insert(out.end(), nal, nal + len);
  }

  std::vector<uint8_t> primed_sps_;
  std::vector<uint8_t> primed_pps_;
};

// Standard base64 (RFC 4648) decode; ignores '=' padding and whitespace.
std::vector<uint8_t> base64Decode(std::string_view in);

// Splits "<b64>,<b64>,..." and base64-decodes each into a raw NAL (RFC 6184
// sprop-parameter-sets order: SPS first, then PPS).
std::vector<std::vector<uint8_t>> parseSpropParameterSets(std::string_view sprop);

// Parse an SDP sprop-parameter-sets value ("<b64>,<b64>,...") and prime `norm`
// with the FIRST SPS and FIRST PPS it carries. Primes only when BOTH are found
// (priming is idempotent); a value missing either set leaves `norm` untouched.
void primeNormalizerFromSprop(H264AnnexBNormalizer& norm, std::string_view sprop_base64_csv);

// Builds a PJ.VideoFrame (format "h264") from the Annex-B access unit and pushes
// it through the runtime host on the given parser binding. Owns the bytes via a
// shared_ptr so the fetcher stays valid after onPoll() returns. Call ONLY from
// onPoll() (the poll thread).
Status pushVideoFrame(
    const PJ::DataSourceRuntimeHostView& runtime_host, PJ::ParserBindingHandle binding, const EncodedFrame& frame,
    std::string_view frame_id);

}  // namespace webrtc
}  // namespace PJ
