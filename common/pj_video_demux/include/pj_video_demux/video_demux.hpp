// pj_video_demux — lazy, non-resident container demux for DataSource plugins.
//
// Splits a video container into a per-access-unit INDEX (timestamps + byte
// locator + keyframe flag) WITHOUT decoding or buffering the bitstream, then
// serves one access unit at a time on demand. A producer pushes each unit as a
// lazy PJ.VideoFrame: the whole compressed video never lands on the heap — only
// the small index and the codec parameter sets stay resident.
//
// Codecs: H.264 and H.265/HEVC in MP4/MOV (length-prefixed NAL samples,
// parameter sets in the avcC/hvcC config box) and AV1 (MP4 OBU samples, sequence
// header in the av1C config box). indexFile() rejects any other codec. Only
// indexFile() touches libav; the fetch path is pure I/O + byte rewrite (no
// libav), mirroring the data_load_mcap locator pattern.

#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <pj_base/buffer_anchor.hpp>
#include <pj_base/builtin/video_frame.hpp>
#include <pj_base/builtin/video_frame_codec.hpp>
#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace PJ {
namespace video_demux {

/// One compressed access unit located within the source container file. The
/// payload is read on demand from `[file_offset, file_offset + size)`; it is the
/// raw container sample — length-prefixed NALs (H.264/HEVC) or OBUs (AV1) — and
/// is rewritten to a self-decodable access unit before use.
struct AccessUnit {
  int64_t dts_ns = 0;       ///< Decode timestamp (ns, monotonic) — the ObjectStore key.
  int64_t pts_ns = 0;       ///< Presentation timestamp (ns) — VideoFrame.timestamp.
  int64_t file_offset = 0;  ///< Byte offset of the packet in the file (libav pkt->pos).
  int32_t size = 0;         ///< Packet size in bytes (libav pkt->size).
  bool keyframe = false;    ///< True for IDR/IRAP/AV1-keyframe access units.
};

/// Result of indexing a container once. `units` are in decode (DTS) order.
/// `param_sets` is the codec parameter data prepended to each keyframe so it is
/// self-decodable: Annex-B SPS/PPS (H.264) or VPS/SPS/PPS (HEVC), or the raw
/// sequence-header configOBUs (AV1). `nal_length_size` is the avcC/hvcC NAL
/// length-prefix width (1/2/4) used to walk each length-prefixed sample; it is
/// unused for AV1.
struct VideoIndex {
  std::string format;               ///< Codec id, lowercase: "h264" | "h265" | "av1".
  std::vector<AccessUnit> units;    ///< Decode-order access-unit index.
  std::vector<uint8_t> param_sets;  ///< Per-keyframe prefix (Annex-B param sets, or AV1 seq OBUs).
  int nal_length_size = 4;          ///< AVCC/HVCC length-prefix width in bytes (unused for av1).
  int width = 0;                    ///< Pixel width (0 = unknown).
  int height = 0;                   ///< Pixel height (0 = unknown).
};

/// Demux a container once (no decode): walk the first video stream's packets,
/// recording per-AU {dts, pts, file offset, size, keyframe}, the codec format,
/// dimensions, and the parameter sets parsed from the config box. Fast — bound
/// by container parsing + I/O, never decodes a frame. Returns an error for
/// codecs other than H.264/HEVC/AV1 or containers whose packets lack byte
/// offsets.
[[nodiscard]] Expected<VideoIndex> indexFile(const std::string& path);

/// Convert one length-prefixed-NAL access unit (AVCC for H.264, HVCC for HEVC —
/// the MP4 sample format is identical) to an Annex-B buffer (4-byte start
/// codes). On a keyframe, `param_sets` (already Annex-B, start-code prefixed) is
/// prepended so the frame is self-decodable — matching the consumer contract
/// that the parameter sets physically precede the IDR/IRAP slice.
/// `nal_length_size` is the config-box length-prefix width. Pure and
/// side-effect free; exposed for unit testing.
[[nodiscard]] std::vector<uint8_t> avccToAnnexB(
    Span<const uint8_t> sample, Span<const uint8_t> param_sets, bool keyframe, int nal_length_size);

/// Assemble one AV1 access unit as a self-contained Low-Overhead-Bitstream-Format
/// temporal unit: the raw MP4 OBU sample, with the sequence-header configOBUs
/// (`seq_obus`) prepended on keyframes (MP4 keeps the seq header in av1C, not in
/// the samples). Non-keyframes pass through unchanged. Pure; exposed for testing.
[[nodiscard]] std::vector<uint8_t> prependAv1SeqHeader(
    Span<const uint8_t> obu_sample, Span<const uint8_t> seq_obus, bool keyframe);

/// Thread-safe, lazily-opened reader that returns the ready-to-decode bytes of a
/// single access unit (Annex-B for H.264/HEVC, an LOBF temporal unit for AV1).
/// Holds only a file handle (opened on first read) — never the file contents.
/// `readUnit` seeks to the unit's `[file_offset, size)`, reads exactly those
/// bytes, and assembles the self-decodable access unit for the index's codec.
///
/// Idempotent and thread-safe (a mutex serializes the shared stream), so it can
/// back a host pushMessage fetcher invoked from any thread, any number of times.
/// Capture it by shared_ptr so it outlives the importData() call that created it.
class LazyAccessUnitReader {
 public:
  /// `path` is the source file; `format`/`param_sets`/`nal_length_size` come from
  /// the VideoIndex of that same file. The file is opened lazily on first read.
  [[nodiscard]] static std::shared_ptr<LazyAccessUnitReader> create(
      std::string path, std::string format, std::vector<uint8_t> param_sets, int nal_length_size);

  /// Read `unit`'s bytes from the file and return them as a self-decodable
  /// access unit for the index's codec. Thread-safe; latches a one-time open
  /// failure.
  [[nodiscard]] Expected<std::vector<uint8_t>> readUnit(const AccessUnit& unit);

 private:
  LazyAccessUnitReader(std::string path, std::string format, std::vector<uint8_t> param_sets, int nal_length_size);

  std::string path_;
  std::string format_;
  std::vector<uint8_t> param_sets_;
  int nal_length_size_;
  std::mutex mutex_;
  std::ifstream stream_;  ///< Opened lazily under `mutex_`.
  bool open_attempted_ = false;
  bool open_failed_ = false;
};

/// Build the lazy host-pushMessage fetcher for one access unit — the per-frame
/// emit primitive shared by every file-backed VideoFrame producer. The returned
/// callable reads `unit` via `reader`, wraps it as a `PJ.VideoFrame`
/// (`frame_id`, `format`, `timestamp_ns = pts_ns`) and serializes it to the
/// canonical wire bytes the host parser expects.
///
/// It THROWS on a read failure so the host fetcher ABI surfaces the precise
/// error (file removed mid-session, short read, …) instead of recording a
/// successful zero-byte frame. Everything is captured by value and the reader is
/// mutex-serialized + idempotent, so the callable is safe to invoke from any
/// host thread any number of times. `pts_ns` is the frame's presentation
/// timestamp; the producer passes the ObjectStore (DTS-based) key separately to
/// `pushMessage`.
[[nodiscard]] inline std::function<PJ::sdk::PayloadView()> makeVideoFrameFetcher(
    std::shared_ptr<LazyAccessUnitReader> reader, AccessUnit unit, std::string format, std::string frame_id,
    int64_t pts_ns) {
  return [reader = std::move(reader), unit, format = std::move(format), frame_id = std::move(frame_id),
          pts_ns]() -> PJ::sdk::PayloadView {
    auto bytes_or = reader->readUnit(unit);
    if (!bytes_or) {
      throw std::runtime_error("pj_video_demux: read failed for '" + frame_id + "': " + bytes_or.error());
    }
    PJ::sdk::VideoFrame frame;
    frame.timestamp_ns = pts_ns;
    frame.frame_id = frame_id;
    frame.format = format;
    frame.data = PJ::Span<const uint8_t>(bytes_or->data(), bytes_or->size());
    auto serialized = std::make_shared<std::vector<uint8_t>>(PJ::serializeVideoFrame(frame));
    return PJ::sdk::PayloadView{serialized};
  };
}

}  // namespace video_demux
}  // namespace PJ
