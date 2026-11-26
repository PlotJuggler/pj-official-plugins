// LeRobot dataset model: parses meta/{info.json,episodes.jsonl,tasks.jsonl} (v2.x) or
// meta/{info.json,episodes/*.parquet,tasks.parquet} (v3.0), gates the codebase version, and exposes a
// version-agnostic episode->shard map. Depends only on the C++ stdlib, nlohmann_json and pj_base/expected.hpp.
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

/// v2.0 and v2.1 share a layout (one parquet per episode, one mp4 per (camera, episode), JSONL metadata) and are
/// both `V2_x`. v3.0 packs many episodes per parquet/mp4 file with relational metadata in chunked parquet.
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
  std::string task_text;  // first task
};

/// v2.x: the mp4 holds exactly that episode, so start_ns / end_ns are `nullopt`. v3.0: the mp4 holds many
/// concatenated episodes, and start_ns / end_ns mark this episode's `[from, to)` offset (ns) inside it.
struct VideoShard {
  std::filesystem::path mp4_path;
  std::optional<int64_t> start_ns;
  std::optional<int64_t> end_ns;
};

/// v2.x: `parquet_path` is the episode's own parquet, `row_from = 0`, `row_to = length`, video windows absent.
/// v3.0: `parquet_path` is the shared shard from `(data_chunk_index, data_file_index)`, `row_from` / `row_to` come
/// from `dataset_from_index` / `dataset_to_index`, and each `VideoShard` carries `from_timestamp` / `to_timestamp`
/// in ns.
struct EpisodeShard {
  std::filesystem::path parquet_path;
  int64_t row_from = 0;
  int64_t row_to = 0;  // half-open: real row count is (row_to - row_from)
  std::vector<std::string> tasks;
  std::unordered_map<std::string, VideoShard> videos;  // keyed by camera name
};

/// The importer consumes the version-agnostic `episode_shards` map and never sees which layout produced it.
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

  /// Parquet holding an episode's frames: the per-episode file (v2.x) or the shared shard (v3.0, where the
  /// importer must slice rows `[episode_shards[id].row_from, .row_to)`).
  [[nodiscard]] std::filesystem::path episodeParquet(int64_t episode_index) const;

  /// The mp4 for one camera: the per-(episode, camera) file (v2.x) or the shared shard (v3.0, where playback must
  /// be restricted to `[episode_shards[id].videos[camera].start_ns, .end_ns)`).
  [[nodiscard]] std::filesystem::path episodeVideo(int64_t episode_index, std::string_view camera) const;
};

/// Walks up from @p picked_path (a file in the dataset or the dataset folder) to the directory containing
/// `meta/info.json`, then parses the meta files. Errors on no dataset, a parse error or an unsupported
/// codebase_version.
[[nodiscard]] PJ::Expected<DatasetModel> loadDatasetModel(const std::filesystem::path& picked_path);

/// Expands a Python-style path template (`{name}` / `{name:0Nd}`): int placeholders are zero-padded per the width
/// spec, string ones substituted verbatim, unknown tokens kept literal. v2.x uses {episode_chunk, episode_index,
/// video_key (or camera_key)}; v3.0 uses {chunk_index, file_index, video_key}.
[[nodiscard]] std::string expandPathTemplate(
    std::string_view tmpl, const std::unordered_map<std::string, int64_t>& int_args,
    const std::unordered_map<std::string, std::string>& str_args = {});

/// v2.x convenience overload with the {episode_chunk, episode_index, video_key/camera_key} placeholders.
[[nodiscard]] std::string expandPathTemplate(
    std::string_view tmpl, int64_t episode_chunk, int64_t episode_index, std::string_view video_key);

}  // namespace lerobot
