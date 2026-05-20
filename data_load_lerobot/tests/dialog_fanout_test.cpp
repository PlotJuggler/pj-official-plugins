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

#include <nlohmann/json.hpp>

#include "lerobot_dialog.hpp"

namespace {

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

}  // namespace
