// LeRobotDialog's fanout serialization. Loading a per-instance fanout sub-config (`episode: int`) must populate
// singleEpisode() so importData reads exactly one episode. The dialog lives in an anonymous namespace in the
// header, so this test has its own copy of the class.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <pj_utf8_path/utf8_path.hpp>
#include <string>
#include <vector>

#include "lerobot_dialog.hpp"

namespace {

namespace fs = std::filesystem;

// Minimal valid v2.1 dataset tree with `episodes` episodes (indices 0..N-1) under a fresh temp dir whose folder
// name is the dataset name we expect to surface (deliberately not "info").
fs::path makeDatasetFixture(const std::string& prefix = "pusht_v21_dialogfix", int episodes = 1) {
  const fs::path root =
      fs::temp_directory_path() /
      PJ::utf8_path::pathFromUtf8(prefix + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
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
  std::ofstream episodes_file(root / "meta" / "episodes.jsonl");
  for (int i = 0; i < episodes; ++i) {
    episodes_file << R"({"episode_index": )" << i << R"(, "tasks": [0], "length": 42})" << "\n";
  }
  return root;
}

TEST(LeRobotDialogFanout, LoadsSingleEpisodeFromFanoutConfig) {
  LeRobotDialog d;
  // Per-instance fanout payload; filepath is empty so loadDatasetModel fails (model parsing is not under test).
  ASSERT_TRUE(d.loadConfig(R"({"episode":5,"display_suffix":"ep_5"})"));
  ASSERT_TRUE(d.singleEpisode().has_value());
  EXPECT_EQ(*d.singleEpisode(), 5);
}

TEST(LeRobotDialogFanout, DialogUiConfigHasNoSingleEpisode) {
  LeRobotDialog d;
  // Dialog mode (no `episode`, has `selected_episodes`): with no dataset loaded the selection stays empty; what
  // matters is that singleEpisode() remains nullopt.
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
  // Empty selection -> no fanout key (the host treats absence as a single instance).
  EXPECT_FALSE(j.contains("__pj_fanout"));
}

TEST(LeRobotDialogFanout, IgnoresLegacyVideoModeKey) {
  // Unknown keys (e.g. a legacy `video_mode`) are ignored.
  LeRobotDialog d;
  EXPECT_TRUE(d.loadConfig(R"({"episode":0,"video_mode":"jpeg"})"));
  EXPECT_TRUE(d.singleEpisode().has_value());
}

// The dialog names the dataset after its root folder, so the catalog shows "pusht_v21", not the opened file's
// basename ("info").
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

// The host hands the dialog its path as UTF-8; on Windows a narrow conversion reads it in the ANSI code page. The
// dataset name that goes back to the host is UTF-8 too.
TEST(LeRobotDialogFanout, OpensDatasetUnderNonAsciiFolder) {
  const fs::path root = makeDatasetFixture(PJ::utf8_path::utf8String(u8"utf8_\u043f\u0443\u0442\u044c_\u98db\u884c"));
  LeRobotDialog d;
  ASSERT_TRUE(
      d.loadConfig(nlohmann::json{{"filepath", PJ::utf8_path::pathToUtf8(root / "meta" / "info.json")}}.dump()));
  ASSERT_NE(d.model(), nullptr) << d.datasetError();

  const auto j = nlohmann::json::parse(d.saveConfig());
  EXPECT_EQ(j["display_name"].get<std::string>(), PJ::utf8_path::pathToUtf8(root.filename()));

  fs::remove_all(root);
}

std::vector<int64_t> selectedEpisodes(const LeRobotDialog& d) {
  return nlohmann::json::parse(d.saveConfig())["selected_episodes"].get<std::vector<int64_t>>();
}

// PlotJuggler pre-fills the next dataset's dialog with the last config, "filepath" rewritten to the new dataset:
// an episode selection belongs to the dataset it was chosen for, whose indices mean nothing in another.
TEST(LeRobotDialogSelection, KeptForItsDatasetAndNotCarriedToAnother) {
  const fs::path pusht = makeDatasetFixture("pusht_v21_sel", 4);
  const fs::path other = makeDatasetFixture("aloha_v21_sel", 4);
  const auto pusht_info = (pusht / "meta" / "info.json").string();
  const auto other_info = (other / "meta" / "info.json").string();

  // A config without the dataset name keeps its selection.
  LeRobotDialog first;
  ASSERT_TRUE(first.loadConfig(nlohmann::json{{"filepath", pusht_info}, {"selected_episodes", {1, 3}}}.dump()));
  ASSERT_EQ(selectedEpisodes(first), (std::vector<int64_t>{1, 3}));

  auto config = nlohmann::json::parse(first.saveConfig());

  // A layout replay of the same dataset keeps the selection.
  LeRobotDialog replayed;
  ASSERT_TRUE(replayed.loadConfig(config.dump()));
  EXPECT_EQ(selectedEpisodes(replayed), (std::vector<int64_t>{1, 3}));

  // Another dataset whose indices overlap gets the default: every episode.
  config["filepath"] = other_info;
  LeRobotDialog next;
  ASSERT_TRUE(next.loadConfig(config.dump()));
  ASSERT_NE(next.model(), nullptr) << next.datasetError();
  EXPECT_EQ(selectedEpisodes(next), (std::vector<int64_t>{0, 1, 2, 3}));

  fs::remove_all(pusht);
  fs::remove_all(other);
}

}  // namespace
