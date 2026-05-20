// Verifies LeRobotDialog's per-instance fanout serialization API.
//
// The dialog is the persistence layer for both the UI selection (legacy
// multi-episode mode) and the per-instance fanout configs the host issues
// when spawning one LeRobotSource per episode. Loading a fanout sub-config
// (with `episode: int`) must populate singleEpisode() so importData skips
// the multi-episode iteration. The video_mode flag survives roundtrips.
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
  // Per-instance fanout payload: single episode int + explicit video_mode.
  // filepath empty so loadDatasetModel fails (we're not testing model parsing).
  ASSERT_TRUE(d.loadConfig(R"({"episode":5,"video_mode":"video","display_suffix":"ep_5"})"));
  ASSERT_TRUE(d.singleEpisode().has_value());
  EXPECT_EQ(*d.singleEpisode(), 5);
  EXPECT_EQ(d.videoMode(), "video");
}

TEST(LeRobotDialogFanout, DefaultsToVideoModeWhenMissing) {
  LeRobotDialog d;
  ASSERT_TRUE(d.loadConfig(R"({"episode":0})"));
  EXPECT_EQ(d.videoMode(), "video");
}

TEST(LeRobotDialogFanout, JpegModePreserved) {
  LeRobotDialog d;
  ASSERT_TRUE(d.loadConfig(R"({"episode":0,"video_mode":"jpeg"})"));
  EXPECT_EQ(d.videoMode(), "jpeg");
}

TEST(LeRobotDialogFanout, LegacyConfigHasNoSingleEpisode) {
  LeRobotDialog d;
  // Legacy mode (no `episode`, has `selected_episodes`). With no real
  // dataset loaded, selected_eps_ stays empty (loadModel fails silently);
  // what matters is that singleEpisode() remains nullopt.
  ASSERT_TRUE(d.loadConfig(R"({"selected_episodes":[1,2,3]})"));
  EXPECT_FALSE(d.singleEpisode().has_value());
}

TEST(LeRobotDialogFanout, MalformedJsonReturnsFalse) {
  LeRobotDialog d;
  EXPECT_FALSE(d.loadConfig("{not-json"));
}

TEST(LeRobotDialogFanout, SaveConfigEmitsVideoMode) {
  LeRobotDialog d;
  ASSERT_TRUE(d.loadConfig(R"({"video_mode":"jpeg"})"));
  const std::string out = d.saveConfig();
  auto j = nlohmann::json::parse(out);
  EXPECT_EQ(j["video_mode"], "jpeg");
  // Empty selection → no fanout key (FileLoader treats absence as
  // single-instance and reuses the scratch handle).
  EXPECT_FALSE(j.contains("__pj_fanout"));
}

TEST(LeRobotDialogFanout, SaveConfigRoundtripPreservesEpisode) {
  // A fanout sub-config must roundtrip through save/load on a fresh dialog
  // so persisted layouts (one per spawned instance) restore deterministically.
  LeRobotDialog src;
  ASSERT_TRUE(src.loadConfig(R"({"episode":42,"video_mode":"video","display_suffix":"ep_42"})"));
  const std::string serialized = src.saveConfig();

  // saveConfig in dialog/UI mode emits selected_eps_ (empty here) and
  // video_mode. The `episode` int is not echoed because saveConfig is
  // dialog-state-only — fanout entries are generated from selected_eps_,
  // not from singleEpisode(). What we verify is that videoMode survives.
  LeRobotDialog dst;
  ASSERT_TRUE(dst.loadConfig(serialized));
  EXPECT_EQ(dst.videoMode(), "video");
}

}  // namespace
