#pragma once

// blf_frames: reads a Vector BLF file and yields classic-CAN frames. lblf is
// used only in the .cpp, so this header pulls in no lblf types.
//
// Scope (v1): classic CAN (CAN_MESSAGE / CAN_MESSAGE2). CAN FD is out — lblf
// has no CAN-FD parse struct, and the DBC decoder rejects payloads >8 bytes;
// CAN-FD/LIN/other objects are counted in BlfStats::skipped_objects. Timestamps
// are absolute ns = the file's measurement-start time (a Windows SYSTEMTIME,
// treated as UTC) plus the per-object relative timestamp.

#include <cstdint>
#include <functional>
#include <pj_base/expected.hpp>
#include <string>
#include <vector>

namespace blf_detail {

/// One classic-CAN frame decoded from a BLF object.
struct CanFrame {
  std::int64_t ts_ns = 0;     ///< absolute nanoseconds
  std::uint16_t channel = 0;  ///< lblf channel index (1-based)
  std::uint32_t can_id = 0;   ///< 11/29-bit id (extended flag masked off)
  bool extended = false;
  std::vector<std::uint8_t> data;
};

using CanFrameCallback = std::function<void(const CanFrame&)>;

struct BlfStats {
  std::uint64_t can_frames = 0;       ///< classic-CAN frames emitted
  std::uint64_t skipped_objects = 0;  ///< non-classic-CAN objects (CAN FD, LIN, ...)
};

/// Reads `path`, invoking `cb` for each classic-CAN frame. `stats` receives the
/// frame/skip counts. Returns an error if the file cannot be opened/read.
PJ::Status readCanFrames(const std::string& path, const CanFrameCallback& cb, BlfStats& stats);

}  // namespace blf_detail
