#include "timeline.hpp"

#include <cmath>

namespace lerobot {

std::vector<int64_t> computeEpisodeOffsetsNs(
    const std::vector<int64_t>& lengths, double fps, double gap_seconds) {
  const double safe_fps = fps > 0.0 ? fps : 1.0;
  const double gap = gap_seconds > 0.0 ? gap_seconds : 0.0;
  const auto gap_ns = static_cast<int64_t>(std::llround(gap * 1e9));

  std::vector<int64_t> offsets;
  offsets.reserve(lengths.size());
  int64_t offset = 0;
  for (std::size_t i = 0; i < lengths.size(); ++i) {
    offsets.push_back(offset);
    const double duration_s = static_cast<double>(lengths[i]) / safe_fps;
    const auto duration_ns = static_cast<int64_t>(std::llround(duration_s * 1e9));
    offset += duration_ns + gap_ns;
  }
  return offsets;
}

int64_t rowTimestampNs(int64_t offset_ns, bool has_ts, double ts_seconds, int64_t frame_index, double fps) {
  if (has_ts) {
    return offset_ns + static_cast<int64_t>(std::llround(ts_seconds * 1e9));
  }
  const double safe_fps = fps > 0.0 ? fps : 1.0;
  const double rel_s = static_cast<double>(frame_index) / safe_fps;
  return offset_ns + static_cast<int64_t>(std::llround(rel_s * 1e9));
}

}  // namespace lerobot
