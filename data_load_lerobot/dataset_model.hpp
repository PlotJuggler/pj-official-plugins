// LeRobot dataset model: parses meta/{info.json,episodes.jsonl,tasks.jsonl}
// for v2.x or meta/{info.json,episodes/*.parquet,tasks.parquet} for v3.0,
// gates the codebase version, and exposes a version-agnostic episode→shard
// map that the importer consumes without knowing which version it loaded.
//
// Pure: depends only on the C++ stdlib, nlohmann_json and pj_base/expected.hpp.
// No Qt, no Arrow, no plugin host APIs — fully unit-testable in isolation.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pj_base/expected.hpp"

namespace lerobot {

/// On-disk format version of the dataset. v2.0 and v2.1 share the same on-disk
/// layout from the plugin's point of view (one parquet per episode, one mp4
/// per (camera, episode), JSONL metadata) and are both surfaced as `V2_x`.
/// v3.0 packs many episodes per parquet/mp4 file with relational metadata in
/// chunked parquet — see `EpisodeShard` below.
enum class DatasetVersion {
  V2_x,
  V3_0,
};

/// One entry of meta/info.json "features".
struct FeatureSpec {
  std::string name;                // e.g. "observation.state"
  std::string dtype;               // "float32" | "int64" | "bool" | "video" | ...
  std::vector<int64_t> shape;      // e.g. [7] or [224,224,3]
  std::vector<std::string> names;  // sub-field labels; may be empty
  std::string video_codec;         // info["video.codec"] when dtype == "video"

  [[nodiscard]] bool is_video() const {
    return dtype == "video";
  }
};

/// One entry of meta/episodes.jsonl (v2.x) or one row of meta/episodes/*.parquet
/// (v3.0), with its task resolved via tasks.jsonl / tasks.parquet.
struct EpisodeInfo {
  int64_t episode_index = 0;
  int64_t length = 0;     // frame count
  std::string task_text;  // first task (resolved to text)
};

/// Where one episode's video lives, plus the optional in-file clip window.
/// For v2.x the mp4 holds exactly that episode → start_ns / end_ns are
/// `nullopt` (the whole file is the playable window). For v3.0 the mp4
/// holds many concatenated episodes → start_ns / end_ns mark this episode's
/// `[from, to)` offset (ns) inside the shared file.
struct VideoShard {
  std::filesystem::path mp4_path;
  std::optional<int64_t> start_ns;
  std::optional<int64_t> end_ns;
};

/// Where one episode's data lives, plus the row range inside its parquet
/// shard and the per-camera videos. For v2.x: `parquet_path` is the unique
/// episode parquet, `row_from = 0`, `row_to = length`, and each `VideoShard`
/// has absent start/end. For v3.0: `parquet_path` is the (shared) shard
/// resolved from `(data_chunk_index, data_file_index)`, `row_from` /
/// `row_to` come from `dataset_from_index` / `dataset_to_index`, and each
/// `VideoShard` carries `from_timestamp` × 1e9 / `to_timestamp` × 1e9.
struct EpisodeShard {
  std::filesystem::path parquet_path;
  int64_t row_from = 0;
  int64_t row_to = 0;  // half-open: real row count is (row_to - row_from)
  std::vector<std::string> tasks;
  std::unordered_map<std::string, VideoShard> videos;  // keyed by camera name
};

/// Parsed LeRobot dataset (v2.0 / v2.1 / v3.0). The importer consumes the
/// version-agnostic `episode_shards` map and stays oblivious to which on-disk
/// layout produced it.
struct DatasetModel {
  std::filesystem::path root;
  std::string codebase_version;  // raw string from info.json, e.g. "v2.1" / "v3.0"
  DatasetVersion version = DatasetVersion::V2_x;
  double fps = 30.0;
  std::string data_path_tmpl;   // info.json "data_path"
  std::string video_path_tmpl;  // info.json "video_path"
  int64_t chunks_size = 1000;   // info.json "chunks_size"
  std::vector<FeatureSpec> features;
  std::vector<std::string> camera_names;                     // names of features with dtype "video"
  std::vector<EpisodeInfo> episodes;                         // sorted by episode_index (for dialog UI)
  std::unordered_map<int64_t, EpisodeShard> episode_shards;  // physical layout, populated for both versions

  [[nodiscard]] const FeatureSpec* feature(std::string_view name) const;

  /// Absolute path to the parquet file that holds an episode's frames.
  /// In v2.x this is the per-episode parquet (rows 0..length); in v3.0 it is
  /// the consolidated shard parquet, and the importer must additionally
  /// slice rows `[episode_shards[id].row_from, .row_to)`.
  [[nodiscard]] std::filesystem::path episodeParquet(int64_t episode_index) const;

  /// Absolute path to the mp4 that holds an episode's frames for one camera.
  /// In v2.x this is the per-(episode, camera) mp4 (whole file is the clip);
  /// in v3.0 it is the consolidated shard mp4, and the importer must
  /// additionally restrict playback to
  /// `[episode_shards[id].videos[camera].start_ns, .end_ns)`.
  [[nodiscard]] std::filesystem::path episodeVideo(int64_t episode_index, std::string_view camera) const;
};

/// Resolve the dataset root by walking up from @p picked_path (a file the user
/// selected, typically a `data/.../episode_*.parquet`, or the dataset folder
/// itself) until a directory containing `meta/info.json` is found, then parse
/// the three meta files. Returns an error string on any hard failure
/// (no dataset found, parse error, unsupported codebase_version).
[[nodiscard]] PJ::Expected<DatasetModel> loadDatasetModel(const std::filesystem::path& picked_path);

/// Expand a LeRobot path template (Python-style `{name}` / `{name:0Nd}`)
/// against a generic dictionary of placeholders. Int placeholders are zero-
/// padded according to the optional `:0Nd` width spec; string placeholders
/// are substituted verbatim. Unknown tokens are kept literal in the output.
/// v2.x defaults expect placeholders {episode_chunk, episode_index, video_key
/// (or camera_key)}; v3.0 expects {chunk_index, file_index, video_key}.
[[nodiscard]] std::string expandPathTemplate(
    std::string_view tmpl, const std::unordered_map<std::string, int64_t>& int_args,
    const std::unordered_map<std::string, std::string>& str_args = {});

/// v2.x convenience overload — delegates to the generic version with the
/// {episode_chunk, episode_index, video_key/camera_key} placeholders. Kept
/// for backward compatibility with existing callers and unit tests.
[[nodiscard]] std::string expandPathTemplate(
    std::string_view tmpl, int64_t episode_chunk, int64_t episode_index, std::string_view video_key);

}  // namespace lerobot
