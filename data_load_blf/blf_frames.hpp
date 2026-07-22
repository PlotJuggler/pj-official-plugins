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
#include <optional>
#include <pj_base/expected.hpp>
#include <string>
#include <string_view>
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

/// Per-frame callback; return false to stop reading early (e.g. on cancel).
using CanFrameCallback = std::function<bool(const CanFrame&)>;

struct BlfStats {
  std::uint64_t can_frames = 0;       ///< classic-CAN frames emitted
  std::uint64_t skipped_objects = 0;  ///< non-classic-CAN objects (CAN FD, LIN, ...)
  std::uint64_t total_objects = 0;    ///< object count from the file header (0 if absent)
};

/// Reads `path`, invoking `cb` for each classic-CAN frame. `stats` receives the
/// frame/skip counts. Returns an error if the file cannot be opened/read.
PJ::Status readCanFrames(const std::string& path, const CanFrameCallback& cb, BlfStats& stats);

/// Parses a `channel_dbcs` map key ("1".."65535") from a saved config. Returns
/// nullopt for non-numeric, negative, or out-of-range keys — hand-edited
/// configs must not throw or silently wrap onto another channel.
std::optional<std::uint16_t> parseChannelKey(std::string_view key);

/// Relative BLF object timestamp in nanoseconds, honoring the TimeTenMics
/// flag (10-µs ticks vs ns). The tick count is file-controlled: saturates at
/// INT64_MAX instead of overflowing or wrapping negative.
std::int64_t objectTimeNs(std::uint32_t object_flags, std::uint64_t ticks);

}  // namespace blf_detail
