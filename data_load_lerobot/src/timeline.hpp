// Pure timeline-synthesis logic: turn LeRobot per-episode-relative float
// seconds into one continuous int64-ns absolute timeline across the selected
// episodes, with an optional inter-episode gap. No Arrow, no host APIs.
#pragma once

#include <cstdint>
#include <vector>

namespace lerobot {

/// Per-selected-episode start offset (ns) on the synthesized global timeline.
///
/// Uniform accumulator (no special-casing of the first episode):
///   offset_0 = 0
///   offset_{k+1} = offset_k + round(length_k / fps * 1e9) + round(gap_s * 1e9)
///
/// A single-episode selection naturally yields offset 0. `lengths` is in
/// selection order; `fps` must be > 0 (clamped to 1.0 otherwise).
[[nodiscard]] std::vector<int64_t> computeEpisodeOffsetsNs(
    const std::vector<int64_t>& lengths, double fps, double gap_seconds);

/// Absolute ns timestamp of one row.
/// If the parquet has a usable `timestamp` column, pass has_ts=true and the
/// per-episode-relative seconds; otherwise the frame index / fps is used.
[[nodiscard]] int64_t rowTimestampNs(
    int64_t offset_ns, bool has_ts, double ts_seconds, int64_t frame_index, double fps);

}  // namespace lerobot
