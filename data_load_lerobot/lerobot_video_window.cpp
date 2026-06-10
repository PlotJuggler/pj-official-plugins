#include "lerobot_video_window.hpp"

#include <limits>

namespace lerobot {

EmitSlice resolveEmitSlice(
    const std::vector<PJ::video_demux::AccessUnit>& units, std::optional<int64_t> start_ns,
    std::optional<int64_t> end_ns) {
  EmitSlice slice;
  slice.first_idx = 0;
  slice.last_idx = units.empty() ? 0 : units.size() - 1;
  slice.origin_ns = units.empty() ? 0 : units.front().dts_ns;
  if (units.empty() || !start_ns.has_value()) {
    return slice;  // v2.x: whole file, rebase to first DTS.
  }

  const int64_t start = *start_ns;
  const int64_t end = end_ns.value_or(std::numeric_limits<int64_t>::max());
  slice.origin_ns = start;

  // Last keyframe at-or-before start (the GOP that covers the window start);
  // falls back to unit 0 (itself a keyframe) when start precedes it.
  slice.first_idx = 0;
  for (std::size_t i = 0; i < units.size(); ++i) {
    if (units[i].keyframe && units[i].pts_ns <= start) {
      slice.first_idx = i;
    }
  }
  // Contiguous through the last unit presented before end, so any reordered /
  // B-frame inside the window stays decodable.
  slice.last_idx = slice.first_idx;
  for (std::size_t i = slice.first_idx; i < units.size(); ++i) {
    if (units[i].pts_ns < end) {
      slice.last_idx = i;
    }
  }
  return slice;
}

}  // namespace lerobot
