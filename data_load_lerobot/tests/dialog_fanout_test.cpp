// Verifies LeRobotDialog's per-instance fanout serialization API.
//
// The dialog persists both the UI selection (multi-episode list) and the
// per-instance fanout configs the host issues when spawning one LeRobotSource
// per episode. Loading a fanout sub-config (with `episode: int`) must populate
// singleEpisode() so importData reads exactly one episode.
//
// Note: the dialog is defined in an anonymous namespace inside the header,
// so we get our own copy of the class here — fine for unit testing, since
// the production plugin .so has its own copy via lerobot_plugin.cpp.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "lerobot_dialog.hpp"

namespace {

namespace fs = std::filesystem;

// Minimal valid v2.1 dataset tree under a fresh temp dir whose folder name is
// the dataset name we expect to surface (deliberately not "info").
fs::path makeDatasetFixture() {
  const fs::path root =
      fs::temp_directory_path() /
      fs::path("pusht_v21_dialogfix_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  fs::remove_all(root);
  fs::create_directories(root / "meta");
  std::ofstream(root / "meta" / "info.json") << R"({
    "codebase_version": "v2.1",
    "robot_type": "so101",
    "fps": 20,
    "chunks_size": 1000,
    "data_path": "data/chunk-{episode_chunk:03d}/episode_{episode_index:06d}.parquet",
    "video_path": "videos/chunk-{episode_chunk:03d}/{video_key}/episode_{episode_index:06d}.mp4",
    "features": {
      "observation.state": {"dtype": "float32", "shape": [3], "names": ["a","b","c"]},
      "action": {"dtype": "float32", "shape": [2]}
    }
  })";
  std::ofstream(root / "meta" / "tasks.jsonl") << R"({"task_index": 0, "task": "t"})" << "\n";
  std::ofstream(root / "meta" / "episodes.jsonl") << R"({"episode_index": 0, "tasks": [0], "length": 42})" << "\n";
  return root;
}

TEST(LeRobotDialogFanout, LoadsSingleEpisodeFromFanoutConfig) {
  LeRobotDialog d;
  // Per-instance fanout payload: single episode int. filepath empty so
  // loadDatasetModel fails (we're not testing model parsing here).
  ASSERT_TRUE(d.loadConfig(R"({"episode":5,"display_suffix":"ep_5"})"));
  ASSERT_TRUE(d.singleEpisode().has_value());
  EXPECT_EQ(*d.singleEpisode(), 5);
}

TEST(LeRobotDialogFanout, DialogUiConfigHasNoSingleEpisode) {
  LeRobotDialog d;
  // Dialog/UI mode (no `episode`, has `selected_episodes`). With no real
  // dataset loaded, selected_eps_ stays empty (loadModel fails silently);
  // what matters is that singleEpisode() remains nullopt.
  ASSERT_TRUE(d.loadConfig(R"({"selected_episodes":[1,2,3]})"));
  EXPECT_FALSE(d.singleEpisode().has_value());
}

TEST(LeRobotDialogFanout, MalformedJsonReturnsFalse) {
  LeRobotDialog d;
  EXPECT_FALSE(d.loadConfig("{not-json"));
}

TEST(LeRobotDialogFanout, SaveConfigOmitsFanoutWhenSelectionEmpty) {
  LeRobotDialog d;
  ASSERT_TRUE(d.loadConfig(R"({})"));
  const std::string out = d.saveConfig();
  auto j = nlohmann::json::parse(out);
  // Empty selection → no fanout key (FileLoader treats absence as
  // single-instance and reuses the scratch handle).
  EXPECT_FALSE(j.contains("__pj_fanout"));
}

TEST(LeRobotDialogFanout, IgnoresLegacyVideoModeKey) {
  // Old QSettings configs may carry `video_mode:"jpeg"` or similar from the
  // retired JPEG transcode era. We just ignore unknown keys.
  LeRobotDialog d;
  EXPECT_TRUE(d.loadConfig(R"({"episode":0,"video_mode":"jpeg"})"));
  EXPECT_TRUE(d.singleEpisode().has_value());
}

// issue #98: the dialog names the dataset after its root folder so the host's
// catalog tree shows e.g. "pusht_v21", not the opened file's basename ("info").
TEST(LeRobotDialogFanout, SaveConfigEmitsDisplayNameFromDatasetRoot) {
  const fs::path root = makeDatasetFixture();
  LeRobotDialog d;
  // The user opens meta/info.json; the plugin resolves the dataset root.
  ASSERT_TRUE(d.loadConfig(nlohmann::json{{"filepath", (root / "meta" / "info.json").string()}}.dump()));
  ASSERT_NE(d.model(), nullptr) << d.datasetError();

  const auto j = nlohmann::json::parse(d.saveConfig());
  ASSERT_TRUE(j.contains("display_name"));
  EXPECT_EQ(j["display_name"].get<std::string>(), root.filename().string());
  EXPECT_NE(j["display_name"].get<std::string>(), std::string("info"));

  fs::remove_all(root);
}

}  // namespace
