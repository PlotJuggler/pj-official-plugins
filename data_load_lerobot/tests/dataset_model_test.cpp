#include "dataset_model.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace lerobot;  // NOLINT(build/namespaces) — test-local convenience

namespace {

// Writes a minimal but realistic LeRobot v2.1 tree under a fresh temp dir.
// `version` lets a test override codebase_version for the v3 gate case.
fs::path makeFixture(const std::string& version = "v2.1") {
  const fs::path root =
      fs::temp_directory_path() /
      fs::path("lerobot_fixture_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + version);
  fs::remove_all(root);
  fs::create_directories(root / "meta");
  fs::create_directories(root / "data" / "chunk-000");
  fs::create_directories(root / "videos" / "chunk-000" / "observation.images.top");

  std::ofstream(root / "meta" / "info.json") << R"({
    "codebase_version": ")" << version << R"(",
    "robot_type": "so101",
    "fps": 20,
    "chunks_size": 1000,
    "data_path": "data/chunk-{episode_chunk:03d}/episode_{episode_index:06d}.parquet",
    "video_path": "videos/chunk-{episode_chunk:03d}/{video_key}/episode_{episode_index:06d}.mp4",
    "features": {
      "observation.state": {"dtype": "float32", "shape": [3], "names": ["a","b","c"]},
      "action": {"dtype": "float32", "shape": [2]},
      "observation.images.top": {"dtype": "video", "shape": [224,224,3]}
    }
  })";

  std::ofstream(root / "meta" / "tasks.jsonl") << R"({"task_index": 0, "task": "grab the cube"})"
                                               << "\n";
  std::ofstream(root / "meta" / "episodes.jsonl")
      << R"({"episode_index": 1, "tasks": ["grab the cube"], "length": 50})" << "\n"
      << R"({"episode_index": 0, "tasks": [0], "length": 42})" << "\n";

  std::ofstream(root / "data" / "chunk-000" / "episode_000000.parquet") << "PAR1";
  return root;
}

}  // namespace

TEST(ExpandPathTemplate, ZeroPadsAndSubstitutes) {
  // Arrange
  const std::string tmpl = "videos/chunk-{episode_chunk:03d}/{video_key}/episode_{episode_index:06d}.mp4";

  // Act
  const std::string out = expandPathTemplate(tmpl, 0, 7, "cam_high");

  // Assert
  EXPECT_EQ(out, "videos/chunk-000/cam_high/episode_000007.mp4");
}

TEST(LoadDatasetModel, ParsesInfoEpisodesAndTasks) {
  // Arrange
  const fs::path root = makeFixture();

  // Act
  auto model = loadDatasetModel(root / "meta" / "info.json");

  // Assert
  ASSERT_TRUE(model.has_value()) << (model.has_value() ? "" : model.error());
  EXPECT_EQ(model->codebase_version, "v2.1");
  EXPECT_DOUBLE_EQ(model->fps, 20.0);
  ASSERT_EQ(model->episodes.size(), 2u);
  EXPECT_EQ(model->episodes[0].episode_index, 0);  // sorted ascending
  EXPECT_EQ(model->episodes[0].length, 42);
  EXPECT_EQ(model->episodes[0].task_text, "grab the cube");  // resolved via tasks.jsonl index
  EXPECT_EQ(model->episodes[1].task_text, "grab the cube");  // inline string
  ASSERT_EQ(model->camera_names.size(), 1u);
  EXPECT_EQ(model->camera_names[0], "observation.images.top");
  const FeatureSpec* cam = model->feature("observation.images.top");
  ASSERT_NE(cam, nullptr);
  EXPECT_TRUE(cam->is_video());
  const FeatureSpec* state = model->feature("observation.state");
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->names, (std::vector<std::string>{"a", "b", "c"}));

  fs::remove_all(root);
}

TEST(LoadDatasetModel, WalksUpFromParquetFile) {
  // Arrange
  const fs::path root = makeFixture();

  // Act — user picked a deep episode parquet, not meta/info.json
  auto model = loadDatasetModel(root / "data" / "chunk-000" / "episode_000000.parquet");

  // Assert
  ASSERT_TRUE(model.has_value()) << (model.has_value() ? "" : model.error());
  EXPECT_EQ(model->root, root);
  EXPECT_EQ(model->episodeParquet(0), root / "data" / "chunk-000" / "episode_000000.parquet");
  EXPECT_EQ(
      model->episodeVideo(1, "observation.images.top"),
      root / "videos" / "chunk-000" / "observation.images.top" / "episode_000001.mp4");

  fs::remove_all(root);
}

// The old `RejectsV3WithClearMessage` test was removed when v3.0 became a
// supported on-disk layout. End-to-end v3.0 loading is exercised by the
// new fixture in dataset_model_test_v3 (see E9): it builds a real
// meta/episodes/chunk-000/file-000.parquet plus a multi-episode
// data/chunk-000/file-000.parquet with Arrow, and asserts
// `model.version == DatasetVersion::V3_0` plus a populated `episode_shards`.

TEST(LoadDatasetModel, FailsWhenNotADataset) {
  // Act
  auto model = loadDatasetModel(fs::temp_directory_path() / "definitely_not_a_lerobot_dataset_xyz" / "foo.parquet");

  // Assert
  ASSERT_FALSE(model.has_value());
  EXPECT_NE(model.error().find("not a LeRobot dataset"), std::string::npos);
}

TEST(LoadDatasetModel, ParsesDictFormFeatureNames) {
  // Arrange: real lerobot/pusht v2.1 stores names as {"motors": [...]}, not [...]
  const fs::path root = fs::temp_directory_path() / "lerobot_dictnames_fixture";
  fs::remove_all(root);
  fs::create_directories(root / "meta");
  std::ofstream(root / "meta" / "info.json") << R"({
    "codebase_version": "v2.1", "fps": 10, "chunks_size": 1000,
    "data_path": "data/chunk-{episode_chunk:03d}/episode_{episode_index:06d}.parquet",
    "video_path": "videos/chunk-{episode_chunk:03d}/{video_key}/episode_{episode_index:06d}.mp4",
    "features": {
      "observation.state": {"dtype":"float32","shape":[2],"names":{"motors":["motor_0","motor_1"]}}
    }
  })";
  std::ofstream(root / "meta" / "episodes.jsonl") << R"({"episode_index": 0, "tasks": ["push"], "length": 5})" << "\n";

  // Act
  auto model = loadDatasetModel(root / "meta" / "info.json");

  // Assert
  ASSERT_TRUE(model.has_value()) << (model.has_value() ? "" : model.error());
  const FeatureSpec* st = model->feature("observation.state");
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->names, (std::vector<std::string>{"motor_0", "motor_1"}));

  fs::remove_all(root);
}
