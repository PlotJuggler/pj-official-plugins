// v3.0 episode-shard reader. Reads `meta/episodes/chunk-XXX/file-NNN.parquet`
// with Arrow + Parquet and populates `DatasetModel::{episodes, episode_shards}`.
// The implementation lives in its own translation unit so the pure-stdlib
// `dataset_model.cpp` stays Arrow-free (and unit-testable in isolation).
#pragma once

#include <filesystem>

#include "pj_base/expected.hpp"

namespace lerobot {

struct DatasetModel;

/// Populate `model.episodes` and `model.episode_shards` from the v3.0
/// `meta/episodes/` chunked Parquet under `root`. The function expects
/// `model.{root, data_path_tmpl, video_path_tmpl, camera_names, fps}` to be
/// set by the caller (parsed from `meta/info.json`).
[[nodiscard]] PJ::Status loadV3EpisodeShards(const std::filesystem::path& root, DatasetModel& model);

}  // namespace lerobot
