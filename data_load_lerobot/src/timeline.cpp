#include "timeline.hpp"

#include <cmath>

namespace lerobot {

int64_t rowTimestampNs(bool has_ts, double ts_seconds, int64_t frame_index, double fps) {
  if (has_ts) {
    return static_cast<int64_t>(std::llround(ts_seconds * 1e9));
  }
  const double safe_fps = fps > 0.0 ? fps : 1.0;
  const double rel_s = static_cast<double>(frame_index) / safe_fps;
  return static_cast<int64_t>(std::llround(rel_s * 1e9));
}

}  // namespace lerobot
