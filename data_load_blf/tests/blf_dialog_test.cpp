#include "../blf_dialog.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

using blf_detail::BlfDialog;

// The host replaces the source config with the accepted dialog's saveConfig()
// (PJ4 FileLoader post-dialog reload). BlfSource supports several DBCs per
// channel, so the dialog must not truncate a channel's list to its first entry.
TEST(BlfDialog, LoadConfigKeepsAllDbcsPerChannel) {
  BlfDialog dialog;
  const std::string config =
      nlohmann::json{{"filepath", ""}, {"channel_dbcs", {{"1", {"powertrain.dbc", "diag.dbc"}}}}}.dump();
  ASSERT_TRUE(dialog.loadConfig(config));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  ASSERT_TRUE(saved.contains("channel_dbcs"));
  EXPECT_EQ(
      saved["channel_dbcs"].value("1", nlohmann::json::array()), nlohmann::json::array({"powertrain.dbc", "diag.dbc"}));
}

// The source primes its borrowed dialog programmatically (the DialogEngine
// never feeds the source config into the dialog), so the setter path must
// reach saveConfig too.
TEST(BlfDialog, SetChannelDbcsReflectsInSaveConfig) {
  BlfDialog dialog;
  dialog.setChannelDbcs({{2, {"bus.dbc", "extra.dbc"}}});

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["channel_dbcs"].value("2", nlohmann::json::array()), nlohmann::json::array({"bus.dbc", "extra.dbc"}));
}

// A typo'd key in a hand-edited config must not throw across the plugin
// boundary or fail the whole load — the bad entry is skipped, the rest kept.
TEST(BlfDialog, InvalidChannelKeysAreSkippedNotFatal) {
  BlfDialog dialog;
  const std::string config =
      nlohmann::json{
          {"filepath", ""}, {"channel_dbcs", {{"ch1", {"typo.dbc"}}, {"70000", {"wrap.dbc"}}, {"2", {"good.dbc"}}}}}
          .dump();
  ASSERT_TRUE(dialog.loadConfig(config));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  ASSERT_TRUE(saved.contains("channel_dbcs"));
  EXPECT_EQ(saved["channel_dbcs"].size(), 1u);  // neither "ch1" nor a wrapped "70000"
  EXPECT_EQ(saved["channel_dbcs"].value("2", nlohmann::json::array()), nlohmann::json::array({"good.dbc"}));
}

// Picking a DBC in the UI deliberately replaces that channel's whole list —
// the single file-picker per channel edits the channel, it does not append.
TEST(BlfDialog, FileSelectionReplacesChannelList) {
  BlfDialog dialog;
  dialog.setChannelDbcs({{1, {"old_a.dbc", "old_b.dbc"}}});
  ASSERT_TRUE(dialog.onFileSelected("buttonDbcCh1", "new.dbc"));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["channel_dbcs"].value("1", nlohmann::json::array()), nlohmann::json::array({"new.dbc"}));
}

}  // namespace
