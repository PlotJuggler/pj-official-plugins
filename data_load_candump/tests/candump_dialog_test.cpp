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
  // Exactly 15 chars is still valid (boundary).
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

TEST(CandumpDialogConfig, OutOfRangeTimeModeFallsBackToAuto) {
  CandumpDialog dialog;
  const std::string config = nlohmann::json{{"filepath", ""}, {"time_mode", 99}}.dump();
  ASSERT_TRUE(dialog.loadConfig(config));
  EXPECT_FALSE(dialog.timeModeOverride().has_value());
}

TEST(CandumpDialogPicker, FileSelectionReplacesInterfaceList) {
  CandumpDialog dialog;
  dialog.setFilePath(testDataPath("log_format.log"));
  ASSERT_TRUE(dialog.onIndexChanged("comboInterface", 0));
  ASSERT_TRUE(dialog.onFileSelected("buttonDictionary", "new.dbc"));

  const auto saved = nlohmann::json::parse(dialog.saveConfig());
  const std::string first_iface = saved["iface_dicts"].items().begin().key();
  EXPECT_EQ(saved["iface_dicts"].value(first_iface, nlohmann::json::array()), nlohmann::json::array({"new.dbc"}));

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
}

TEST(CandumpDialogPrescan, SummarizesLogFormatFixture) {
  CandumpDialog dialog;
  dialog.setFilePath(testDataPath("log_format.log"));
  const std::string widget_json = dialog.widget_data();
  const auto parsed = nlohmann::json::parse(widget_json);
  const std::string summary = parsed["labelSummary"]["text"].get<std::string>();
  EXPECT_NE(summary.find("log format"), std::string::npos) << summary;
  EXPECT_NE(summary.find("absolute"), std::string::npos) << summary;

  // Two interfaces in the fixture: "can0" and "can-eth0.1".
  ASSERT_TRUE(parsed.contains("comboInterface"));
  const auto items = parsed["comboInterface"]["items"];
  ASSERT_EQ(items.size(), 2u);
  EXPECT_NE(std::find(items.begin(), items.end(), "can0"), items.end());
  EXPECT_NE(std::find(items.begin(), items.end(), "can-eth0.1"), items.end());
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
