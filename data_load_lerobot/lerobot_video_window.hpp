// Pure resolution of which video access units a LeRobot episode emits, and the
// origin used to rebase their timestamps to an episode-local 0. Split out of the
// plugin (which needs a host) so the tricky v3.0 windowing — keyframe-seek-back,
// presentation window, rebase — is unit-testable against synthetic indices.
//
// Depends only on the (header-only, ffmpeg-free) pj_video_demux AccessUnit POD
// and the C++ stdlib: no host APIs, no libav.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <pj_video_demux/video_demux.hpp>
#include <vector>

namespace lerobot {

/// The decode-order slice `[first_idx, last_idx]` of a VideoIndex to emit for one
/// episode, plus the `origin_ns` subtracted from each unit's dts/pts to land the
/// episode-local timeline on 0.
struct EmitSlice {
  std::size_t first_idx = 0;
  std::size_t last_idx = 0;
  int64_t origin_ns = 0;
};

/// Resolve the emit slice for an episode over a (possibly shared) video file.
/// `units` MUST be non-empty and in decode (DTS) order.
///
/// - No window (`start_ns == std::nullopt`): the whole file is the episode
///   (v2.x). Slice = all units; origin = first unit's DTS (rebase to ~0).
/// - With a window (v3.0): `[start_ns, end_ns)` in the file's presentation (PTS)
///   clock; `end_ns == std::nullopt` means "to end of file". The slice starts at
///   the keyframe at-or-before `start_ns` (so a mid-GOP window still decodes; the
///   pre-window frames carry negative episode-local timestamps the tracker never
///   visits) and runs contiguously through the last unit presented before
///   `end_ns` (keeping any reordered/B-frame inside the window decodable).
///   origin = `start_ns`, so the frame at the window start lands on 0. Assumes
///   closed-GOP random-access keyframes (no leading pictures presented before
///   the seek-back keyframe), as the streaming VideoFrame contract intends.
[[nodiscard]] EmitSlice resolveEmitSlice(
    const std::vector<PJ::video_demux::AccessUnit>& units, std::optional<int64_t> start_ns,
    std::optional<int64_t> end_ns);

}  // namespace lerobot
