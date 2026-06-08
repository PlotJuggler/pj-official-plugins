// pj_video_demux — lazy, non-resident container demux for DataSource plugins.
//
// Splits a video container into a per-access-unit INDEX (timestamps + byte
// locator + keyframe flag) WITHOUT decoding or buffering the bitstream, then
// serves one access unit at a time on demand. A producer pushes each unit as a
// lazy PJ.VideoFrame: the whole compressed video never lands on the heap — only
// the small index and the SPS/PPS parameter sets stay resident.
//
// Spike scope: H.264 in an MP4/MOV container (AVCC, parameter sets in `avcC`).
// indexFile() rejects other codecs for now; multi-codec (h265/av1/vp9) is a
// later phase. The fetch path is pure I/O + byte rewrite (no libav), mirroring
// the data_load_mcap locator pattern.

#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>
#include <string>
#include <vector>

namespace PJ {
namespace video_demux {

/// One compressed access unit located within the source container file. The
/// payload is read on demand from `[file_offset, file_offset + size)`; it is
/// AVCC (length-prefixed NALs) and must be rewritten to Annex-B before use.
struct AccessUnit {
  int64_t dts_ns = 0;       ///< Decode timestamp (ns, monotonic) — the ObjectStore key.
  int64_t pts_ns = 0;       ///< Presentation timestamp (ns) — VideoFrame.timestamp.
  int64_t file_offset = 0;  ///< Byte offset of the packet in the file (libav pkt->pos).
  int32_t size = 0;         ///< Packet size in bytes (libav pkt->size).
  bool keyframe = false;    ///< True for IDR/keyframe access units.
};

/// Result of indexing a container once. `units` are in decode (DTS) order;
/// `annexb_params` is the SPS+PPS as an Annex-B blob (start-code prefixed),
/// prepended to keyframes so each is self-decodable; `nal_length_size` is the
/// avcC NAL length-prefix width (1/2/4) used to walk each AVCC access unit.
struct VideoIndex {
  std::string format;                  ///< Codec id, lowercase. "h264" for now.
  std::vector<AccessUnit> units;       ///< Decode-order access-unit index.
  std::vector<uint8_t> annexb_params;  ///< SPS+PPS as Annex-B (start codes).
  int nal_length_size = 4;             ///< AVCC length-prefix width in bytes.
  int width = 0;                       ///< Pixel width (0 = unknown).
  int height = 0;                      ///< Pixel height (0 = unknown).
};

/// Demux a container once (no decode): walk the first video stream's packets,
/// recording per-AU {dts, pts, file offset, size, keyframe}, the codec format,
/// dimensions, and the Annex-B SPS/PPS parsed from the avcC extradata. Fast —
/// bound by container parsing + I/O, never decodes a frame. Returns an error
/// for non-H.264 streams or containers whose packets lack byte offsets.
[[nodiscard]] Expected<VideoIndex> indexFile(const std::string& path);

/// Convert one AVCC access unit (length-prefixed NALs) to an Annex-B buffer
/// (4-byte start codes). On a keyframe, `annexb_params` (SPS+PPS, already
/// start-code prefixed) is prepended so the frame is self-decodable — matching
/// the consumer contract that SPS/PPS physically precede the IDR slice.
/// `nal_length_size` is the avcC length-prefix width. Pure and side-effect
/// free; exposed for unit testing.
[[nodiscard]] std::vector<uint8_t> avccToAnnexB(
    Span<const uint8_t> avcc, Span<const uint8_t> annexb_params, bool keyframe, int nal_length_size);

/// Thread-safe, lazily-opened reader that returns the Annex-B bytes of a single
/// access unit. Holds only a file handle (opened on first read) — never the file
/// contents. `readUnit` seeks to the unit's `[file_offset, size)`, reads exactly
/// those bytes, and rewrites them to Annex-B (prepending SPS/PPS on keyframes).
///
/// Idempotent and thread-safe (a mutex serializes the shared stream), so it can
/// back a host pushMessage fetcher invoked from any thread, any number of times.
/// Capture it by shared_ptr so it outlives the importData() call that created it.
class LazyAnnexBReader {
 public:
  /// `path` is the source file; `annexb_params`/`nal_length_size` come from the
  /// VideoIndex of that same file. The file is opened lazily on the first read.
  [[nodiscard]] static std::shared_ptr<LazyAnnexBReader> create(
      std::string path, std::vector<uint8_t> annexb_params, int nal_length_size);

  /// Read `unit`'s bytes from the file and return them as an Annex-B access
  /// unit. Thread-safe; latches a one-time open failure.
  [[nodiscard]] Expected<std::vector<uint8_t>> readUnit(const AccessUnit& unit);

 private:
  LazyAnnexBReader(std::string path, std::vector<uint8_t> annexb_params, int nal_length_size);

  std::string path_;
  std::vector<uint8_t> annexb_params_;
  int nal_length_size_;
  std::mutex mutex_;
  std::ifstream stream_;  ///< Opened lazily under `mutex_`.
  bool open_attempted_ = false;
  bool open_failed_ = false;
};

}  // namespace video_demux
}  // namespace PJ
