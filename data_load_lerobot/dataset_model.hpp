// LeRobot dataset model: parses meta/{info.json,episodes.jsonl,tasks.jsonl},
// gates the codebase version, and expands the data/video path templates.
//
// Pure: depends only on the C++ stdlib, nlohmann_json and pj_base/expected.hpp.
// No Qt, no Arrow, no plugin host APIs — fully unit-testable in isolation.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"

namespace lerobot {

/// One entry of meta/info.json "features".
struct FeatureSpec {
  std::string name;                // e.g. "observation.state"
  std::string dtype;               // "float32" | "int64" | "bool" | "video" | ...
  std::vector<int64_t> shape;      // e.g. [7] or [224,224,3]
  std::vector<std::string> names;  // sub-field labels; may be empty

  [[nodiscard]] bool is_video() const {
    return dtype == "video";
  }
};

/// One entry of meta/episodes.jsonl, with its task resolved via tasks.jsonl.
struct EpisodeInfo {
  int64_t episode_index = 0;
  int64_t length = 0;     // frame count
  std::string task_text;  // first task (resolved to text)
};

/// Parsed LeRobot dataset (v2.0 / v2.1). v3.0 is rejected by loadDatasetModel.
struct DatasetModel {
  std::filesystem::path root;
  std::string codebase_version;  // "v2.0" | "v2.1"
  double fps = 30.0;
  std::string data_path_tmpl;   // info.json "data_path"
  std::string video_path_tmpl;  // info.json "video_path"
  int64_t chunks_size = 1000;   // info.json "chunks_size"
  std::vector<FeatureSpec> features;
  std::vector<std::string> camera_names;  // names of features with dtype "video"
  std::vector<EpisodeInfo> episodes;      // sorted by episode_index

  [[nodiscard]] const FeatureSpec* feature(std::string_view name) const;

  /// Absolute path to the per-episode parquet file.
  [[nodiscard]] std::filesystem::path episodeParquet(int64_t episode_index) const;

  /// Absolute path to the per-episode, per-camera mp4 file.
  [[nodiscard]] std::filesystem::path episodeVideo(int64_t episode_index, std::string_view camera) const;
};

/// Resolve the dataset root by walking up from @p picked_path (a file the user
/// selected, typically a `data/.../episode_*.parquet`, or the dataset folder
/// itself) until a directory containing `meta/info.json` is found, then parse
/// the three meta files. Returns an error string on any hard failure
/// (no dataset found, parse error, unsupported codebase_version).
[[nodiscard]] PJ::Expected<DatasetModel> loadDatasetModel(const std::filesystem::path& picked_path);

/// Expand a LeRobot path template (Python-style `{name}` / `{name:0Nd}`)
/// against episode_chunk / episode_index (ints) and video_key (string).
/// Exposed for unit testing.
[[nodiscard]] std::string expandPathTemplate(
    std::string_view tmpl, int64_t episode_chunk, int64_t episode_index, std::string_view video_key);

}  // namespace lerobot
