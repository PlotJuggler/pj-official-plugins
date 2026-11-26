// v3.0 episode-shard reader (`meta/episodes/chunk-XXX/file-NNN.parquet`). Kept apart from `dataset_model.cpp` so
// that stays Arrow-free.
#pragma once

#include <filesystem>

#include "pj_base/expected.hpp"

namespace lerobot {

struct DatasetModel;

/// Populates `model.episodes` and `model.episode_shards`. The caller must have set
/// `model.{root, data_path_tmpl, video_path_tmpl, camera_names, fps}` from `meta/info.json`.
[[nodiscard]] PJ::Status loadV3EpisodeShards(const std::filesystem::path& root, DatasetModel& model);

}  // namespace lerobot
