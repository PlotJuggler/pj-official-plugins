// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "video_emit.hpp"

#include <memory>
#include <pj_base/builtin/video_frame.hpp>
#include <pj_base/builtin/video_frame_codec.hpp>
#include <utility>

namespace PJ {
namespace webrtc {

bool H264AnnexBNormalizer::containsIdr(const std::vector<uint8_t>& annexb) {
  AnnexBIterator it(annexb.data(), annexb.size());
  for (NalView v = it.next(); v.data != nullptr; v = it.next()) {
    if (v.type() == kNalSliceIdr) {
      return true;
    }
  }
  return false;
}

std::vector<uint8_t> H264AnnexBNormalizer::normalize(const uint8_t* au, size_t size, bool& out_keyframe) const {
  std::vector<NalSpan> nals;
  if (looksAnnexB(au, size)) {
    AnnexBIterator it(au, size);
    for (NalView v = it.next(); v.data != nullptr; v = it.next()) {
      if (v.size > 0) {
        nals.push_back({v.data, v.size});
      }
    }
  } else {
    // Length-prefixed (libdatachannel default): 4-byte big-endian length, NAL.
    size_t off = 0;
    while (off + 4 <= size) {
      const uint32_t len = (static_cast<uint32_t>(au[off]) << 24) | (static_cast<uint32_t>(au[off + 1]) << 16) |
                           (static_cast<uint32_t>(au[off + 2]) << 8) | static_cast<uint32_t>(au[off + 3]);
      off += 4;
      if (len == 0 || off + len > size) {
        break;
      }
      nals.push_back({au + off, len});
      off += len;
    }
  }

  bool has_idr = false;
  bool has_sps = false;
  bool has_pps = false;
  for (const auto& n : nals) {
    const uint8_t t = nalType(n.data[0]);
    has_idr = has_idr || (t == kNalSliceIdr);
    has_sps = has_sps || (t == kNalSps);
    has_pps = has_pps || (t == kNalPps);
  }

  std::vector<uint8_t> out;
  // Reserve the whole output once: each NAL gets a 4-byte start code, plus the
  // primed SPS/PPS (with their own start codes) when injected on a keyframe.
  size_t reserve = size + 4 * nals.size();
  if (has_idr) {
    if (!has_sps && !primed_sps_.empty()) {
      reserve += primed_sps_.size() + 4;
    }
    if (!has_pps && !primed_pps_.empty()) {
      reserve += primed_pps_.size() + 4;
    }
  }
  out.reserve(reserve);
  // Keyframe missing in-band parameter sets -> inject primed SPS/PPS first, so
  // the consumer's extractH264SpsPps (which stops at the IDR) finds them.
  if (has_idr) {
    if (!has_sps && !primed_sps_.empty()) {
      appendNal(out, primed_sps_.data(), primed_sps_.size());
    }
    if (!has_pps && !primed_pps_.empty()) {
      appendNal(out, primed_pps_.data(), primed_pps_.size());
    }
  }
  for (const auto& n : nals) {
    appendNal(out, n.data, n.size);
  }
  out_keyframe = has_idr;
  return out;
}

std::vector<uint8_t> base64Decode(std::string_view in) {
  constexpr int8_t kInv = -1;
  auto val = [](char c) -> int8_t {
    if (c >= 'A' && c <= 'Z') {
      return static_cast<int8_t>(c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
      return static_cast<int8_t>(c - 'a' + 26);
    }
    if (c >= '0' && c <= '9') {
      return static_cast<int8_t>(c - '0' + 52);
    }
    if (c == '+') {
      return static_cast<int8_t>(62);
    }
    if (c == '/') {
      return static_cast<int8_t>(63);
    }
    return kInv;
  };
  std::vector<uint8_t> out;
  uint32_t buf = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
      continue;
    }
    const int8_t v = val(c);
    if (v == kInv) {
      continue;  // skip stray chars defensively
    }
    buf = (buf << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFFu));
    }
  }
  return out;
}

std::vector<std::vector<uint8_t>> parseSpropParameterSets(std::string_view sprop) {
  std::vector<std::vector<uint8_t>> nals;
  size_t start = 0;
  while (start <= sprop.size()) {
    const size_t comma = sprop.find(',', start);
    const std::string_view token =
        sprop.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    auto nal = base64Decode(token);
    if (!nal.empty()) {
      nals.push_back(std::move(nal));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return nals;
}

void primeNormalizerFromSprop(H264AnnexBNormalizer& norm, std::string_view sprop_base64_csv) {
  if (sprop_base64_csv.empty()) {
    return;
  }
  auto nals = parseSpropParameterSets(sprop_base64_csv);
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  for (auto& nal : nals) {
    if (nal.empty()) {
      continue;
    }
    const uint8_t t = nalType(nal[0]);
    if (t == kNalSps && sps.empty()) {
      sps = std::move(nal);
    } else if (t == kNalPps && pps.empty()) {
      pps = std::move(nal);
    }
  }
  if (!sps.empty() && !pps.empty()) {
    norm.prime(std::move(sps), std::move(pps));
  }
}

Status pushVideoFrame(
    const PJ::DataSourceRuntimeHostView& runtime_host, PJ::ParserBindingHandle binding, const EncodedFrame& frame,
    std::string_view frame_id) {
  // Build the canonical PJ.VideoFrame and serialize it once. The serialized
  // bytes are owned by a shared_ptr so the pushMessage fetcher remains valid
  // after onPoll() returns (ObjectIngestPolicy may defer/repeat the fetch).
  sdk::VideoFrame vf;
  vf.timestamp_ns = frame.ts_ns;
  vf.frame_id = std::string(frame_id);
  vf.format = "h264";
  vf.data = PJ::Span<const uint8_t>(frame.annexb.data(), frame.annexb.size());
  auto serialized = std::make_shared<std::vector<uint8_t>>(serializeVideoFrame(vf));

  return runtime_host.pushMessage(binding, PJ::Timestamp{frame.ts_ns}, [serialized]() -> PJ::sdk::PayloadView {
    return PJ::sdk::PayloadView{serialized};
  });
}

}  // namespace webrtc
}  // namespace PJ
