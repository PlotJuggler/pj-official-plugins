// Per-row timestamp synthesis: derive an int64 ns timestamp from either the
// parquet `timestamp` column (preferred) or `frame_index / fps`. Episodes
// each become their own DatasetId with a 0-based clock, so no cross-episode
// offset arithmetic is needed.
#pragma once

#include <cstdint>

namespace lerobot {

/// Per-row ns timestamp on the episode's 0-based clock.
/// If the parquet has a usable `timestamp` column, pass has_ts=true and the
/// row's seconds; otherwise the frame index / fps is used.
[[nodiscard]] int64_t rowTimestampNs(bool has_ts, double ts_seconds, int64_t frame_index, double fps);

}  // namespace lerobot
