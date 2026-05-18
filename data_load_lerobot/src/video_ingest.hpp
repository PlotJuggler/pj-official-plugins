// Eager per-frame video ingest: demux a LeRobot episode mp4 with FFmpeg,
// annex-B-convert H.264/H.265, and pushOwned() each frame into an ObjectStore
// topic at the synthesized global timestamp. Mirrors the proven
// pj_scene2D/tests/streaming_video_source_test.cpp pattern. FFmpeg stays in
// the .cpp; this header is dependency-light.
#pragma once

#include <pj_base/sdk/plugin_data_api.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lerobot {

/// Demux `mp4_path` and push every video frame to `topic` via `host`.
/// `frame_ts_ns[k]` is the absolute ns timestamp for the k-th video packet
/// (= the same ts_global as that episode's parquet row k). If there are more
/// packets than entries the last timestamp is reused (clamp); we never assume
/// packet_count == episode length. `codec` comes from info.json
/// `features[cam].info["video.codec"]` ("h264"/"avc1", "hevc"/"hvc1", …).
///
/// Returns an error only on hard demux failures; an empty/decode-less file is
/// reported by the caller. The object topic must already be registered.
[[nodiscard]] PJ::Status ingestEpisodeVideo(
    const PJ::sdk::SourceObjectWriteHostView& host,
    PJ::sdk::ObjectTopicHandle topic,
    const std::string& mp4_path,
    const std::string& codec,
    const std::vector<int64_t>& frame_ts_ns);

}  // namespace lerobot
