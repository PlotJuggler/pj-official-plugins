#include "../candump_dialog.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "test_support.hpp"

namespace {

using candump_detail::CandumpDialog;
using candump_detail::TimeMode;
using candump_test::testDataPath;

TEST(CandumpDialogKeys, ValidatesInterfaceKeys) {
  EXPECT_TRUE(CandumpDialog::isValidInterfaceKey("can0"));
  EXPECT_TRUE(CandumpDialog::isValidInterfaceKey("can-eth0.1"));
  EXPECT_FALSE(CandumpDialog::isValidInterfaceKey(""));
  EXPECT_FALSE(CandumpDialog::isValidInterfaceKey("this-name-is-too-long"));  // > 15 chars
  EXPECT_FALSE(CandumpDialog::isValidInterfaceKey("vcan0/1"));                // '/'
  // Boundary: exactly 15 chars.
  EXPECT_TRUE(CandumpDialog::isValidInterfaceKey("exactly15chars0"));
}

TEST(CandumpDialogConfig, LoadConfigKeepsAllDictsPerInterface) {
  CandumpDialog dialog;
  const std::string config =
      nlohmann::json{{"filepath", ""}, {"iface_dicts", {{"can0", {"powertrain.dbc", "diag.csv"}}}}}.dump();
  ASSERT_TRUE(dialog.loadConfig(config));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  ASSERT_TRUE(saved.contains("iface_dicts"));
  EXPECT_EQ(
      saved["iface_dicts"].value("can0", nlohmann::json::array()),
      nlohmann::json::array({"powertrain.dbc", "diag.csv"}));
}

TEST(CandumpDialogConfig, SetInterfaceDictsReflectsInSaveConfig) {
  CandumpDialog dialog;
  dialog.setInterfaceDicts({{"can1", {"bus.dbc"}}});
  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["iface_dicts"].value("can1", nlohmann::json::array()), nlohmann::json::array({"bus.dbc"}));
}

TEST(CandumpDialogConfig, InvalidInterfaceKeysAreSkippedNotFatal) {
  CandumpDialog dialog;
  const std::string config = nlohmann::json{
      {"filepath", ""},
      {"iface_dicts",
       {{"", {"typo.dbc"}},
        {"this-name-is-too-long", {"wrap.dbc"}},
        {"bad/name", {"slash.dbc"}},
        {"can2", {"good.dbc"}}}}}.dump();
  ASSERT_TRUE(dialog.loadConfig(config));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  ASSERT_TRUE(saved.contains("iface_dicts"));
  EXPECT_EQ(saved["iface_dicts"].size(), 1u);
  EXPECT_EQ(saved["iface_dicts"].value("can2", nlohmann::json::array()), nlohmann::json::array({"good.dbc"}));
}

TEST(CandumpDialogConfig, RawUnassignedAndTimeModeRoundTrip) {
  CandumpDialog dialog;
  const std::string config = nlohmann::json{{"filepath", ""}, {"raw_unassigned", false}, {"time_mode", 3}}.dump();
  ASSERT_TRUE(dialog.loadConfig(config));
  EXPECT_FALSE(dialog.rawUnassigned());
  ASSERT_TRUE(dialog.timeModeOverride().has_value());
  EXPECT_EQ(*dialog.timeModeOverride(), TimeMode::kRelativeDelta);

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved.value("raw_unassigned", true), false);
  EXPECT_EQ(saved.value("time_mode", 0), 3);
}

// The host pre-fills the next file's dialog with the last config, "filepath" rewritten to the new file: a
// time mode forced for one capture must not override the next capture's auto-detection.
TEST(CandumpDialogConfig, TimeModeChosenForAnotherFileFallsBackToAuto) {
  CandumpDialog first;
  first.setFilePath(testDataPath("relative_tz.log"));
  ASSERT_TRUE(first.onIndexChanged("comboTimeMode", 1));  // Absolute
  auto config = nlohmann::json::parse(first.saveConfig());
  config["filepath"] = testDataPath("relative_td_mixed.log");

  CandumpDialog next;
  ASSERT_TRUE(next.loadConfig(config.dump()));
  EXPECT_FALSE(next.timeModeOverride().has_value());
  EXPECT_EQ(nlohmann::json::parse(next.widget_data())["comboTimeMode"].value("current_index", -1), 0);

  // A different name in another folder is another capture too.
  config["filepath"] = "/elsewhere/relative_td_mixed.log";
  CandumpDialog elsewhere;
  ASSERT_TRUE(elsewhere.loadConfigDeferringScan(config.dump()));
  EXPECT_FALSE(elsewhere.timeModeOverride().has_value());
}

// A layout replay keeps the forced mode wherever the capture lies now: the file is recognised by its name, because
// the browser build backs every upload with a fresh path and the host records no __pj_saved_filepath for it.
TEST(CandumpDialogConfig, TimeModeSurvivesLayoutReplayOfItsFile) {
  CandumpDialog first;
  first.setFilePath(testDataPath("relative_tz.log"));
  ASSERT_TRUE(first.onIndexChanged("comboTimeMode", 3));
  const auto config = nlohmann::json::parse(first.saveConfig());

  CandumpDialog same;
  ASSERT_TRUE(same.loadConfig(config.dump()));
  EXPECT_EQ(same.timeModeOverride(), TimeMode::kRelativeDelta);

  for (const char* moved_to : {"/elsewhere/relative_tz.log", "C:\\captures\\relative_tz.log"}) {
    auto moved = config;
    moved["filepath"] = moved_to;
    CandumpDialog replayed;
    ASSERT_TRUE(replayed.loadConfigDeferringScan(moved.dump()));
    EXPECT_EQ(replayed.timeModeOverride(), TimeMode::kRelativeDelta) << moved_to;
  }
}

TEST(CandumpDialogConfig, OutOfRangeTimeModeFallsBackToAuto) {
  CandumpDialog dialog;
  const std::string config = nlohmann::json{{"filepath", ""}, {"time_mode", 99}}.dump();
  ASSERT_TRUE(dialog.loadConfig(config));
  EXPECT_FALSE(dialog.timeModeOverride().has_value());
}

TEST(CandumpDialogPicker, FileSelectionReplacesInterfaceList) {
  CandumpDialog dialog;
  dialog.setFilePath(testDataPath("log_format.log"));
  ASSERT_TRUE(dialog.onSelectionChanged("tableInterfaces", {"can0"}));
  ASSERT_TRUE(dialog.onFileSelected("buttonDictionary", "new.dbc"));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  const std::string first_iface = saved["iface_dicts"].items().begin().key();
  EXPECT_EQ(saved["iface_dicts"].value(first_iface, nlohmann::json::array()), nlohmann::json::array({"new.dbc"}));

  // The table's Dictionary column follows the pick (file name only).
  const auto shown = nlohmann::json::parse(dialog.widget_data());
  EXPECT_EQ(shown["tableInterfaces"]["rows"][0][3], "new.dbc");

  ASSERT_TRUE(dialog.onClicked("buttonClearDictionary"));
  const auto cleared = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_FALSE(cleared["iface_dicts"].contains(first_iface));
}

TEST(CandumpDialogPicker, UnknownWidgetsReturnFalse) {
  CandumpDialog dialog;
  EXPECT_FALSE(dialog.onFileSelected("somethingElse", "path"));
  EXPECT_FALSE(dialog.onClicked("somethingElse"));
  EXPECT_FALSE(dialog.onToggled("somethingElse", true));
  EXPECT_FALSE(dialog.onIndexChanged("somethingElse", 1));
  EXPECT_FALSE(dialog.onSelectionChanged("somethingElse", {"can0"}));
  EXPECT_FALSE(dialog.onSelectionChanged("tableInterfaces", {"no-such-iface"}));
}

TEST(CandumpDialogPrescan, SummarizesLogFormatFixture) {
  CandumpDialog dialog;
  dialog.setFilePath(testDataPath("log_format.log"));
  const std::string widget_json = dialog.widget_data();
  const auto parsed = nlohmann::json::parse(widget_json);
  const std::string summary = parsed["labelSummary"]["text"].get<std::string>();
  EXPECT_NE(summary.find("Format: candump -l"), std::string::npos) << summary;
  EXPECT_NE(summary.find("UTC \xC2\xB7 absolute time"), std::string::npos) << summary;

  // No dictionary assigned yet: the table's Dictionary column says so.
  EXPECT_EQ(parsed["tableInterfaces"]["rows"][0][3], "none");

  // Two interfaces in the fixture: "can0" and "can-eth0.1".
  const auto rows = parsed["tableInterfaces"]["rows"];
  ASSERT_EQ(rows.size(), 2u);
  std::vector<std::string> names;
  for (const auto& row : rows) {
    names.push_back(row[0].get<std::string>());
  }
  EXPECT_NE(std::find(names.begin(), names.end(), "can0"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "can-eth0.1"), names.end());
}

TEST(CandumpDialogPrescan, ReportsNotACandumpFile) {
  CandumpDialog dialog;
  const std::string path = testDataPath("arus_subset.csv");  // valid file, but not candump shaped
  dialog.setFilePath(path);
  const auto parsed = nlohmann::json::parse(dialog.widget_data());
  const std::string summary = parsed["labelSummary"]["text"].get<std::string>();
  EXPECT_NE(summary.find("does not look like a candump capture"), std::string::npos) << summary;
}

TEST(CandumpDialogPrescan, InterfacesSurviveARescanThatDoesNotSeeThem) {
  CandumpDialog dialog;
  dialog.setInterfaceDicts({{"vcan9", {"stale.dbc"}}});
  dialog.setFilePath(testDataPath("log_format.log"));  // does not mention "vcan9"
  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  EXPECT_EQ(saved["iface_dicts"].value("vcan9", nlohmann::json::array()), nlohmann::json::array({"stale.dbc"}));
}

}  // namespace
