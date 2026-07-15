#define MCAP_IMPLEMENTATION
#include "mcap_dialog.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace {

TEST(McapDialogTest, DefaultConstructs) {
  McapDialog dialog;
  EXPECT_TRUE(dialog.selectedTopics().empty());
  EXPECT_TRUE(dialog.analyzeError().empty());
}

TEST(McapDialogTest, WidgetDataSelectsRowsByTextNotIndex) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_publish_vs_log_time.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  const auto data = nlohmann::json::parse(dialog.widget_data());
  ASSERT_TRUE(data.contains("tableWidget"));
  const auto& tbl = data["tableWidget"];

  ASSERT_TRUE(tbl.contains("selected_items"));
  std::vector<std::string> selected_items = tbl["selected_items"].get<std::vector<std::string>>();
  EXPECT_EQ(selected_items, (std::vector<std::string>{"/sensor/value"}));

  EXPECT_FALSE(tbl.contains("selected_rows"));
}

TEST(AlwaysIncludeRuleTest, MatchesRos2Tf) {
  ChannelInfo ch;
  ch.topic = "/tf";
  ch.schema = "tf2_msgs/msg/TFMessage";
  ch.encoding = "cdr";
  ch.msg_count = 10;
  EXPECT_TRUE(isAlwaysIncluded(ch));
}

TEST(AlwaysIncludeRuleTest, MatchesFoxgloveFrameTransformUnderAnyTopicName) {
  ChannelInfo ch;
  ch.topic = "transforms";  // deliberately not "/tf"
  ch.schema = "foxglove.FrameTransform";
  ch.encoding = "protobuf";
  ch.msg_count = 10;
  EXPECT_TRUE(isAlwaysIncluded(ch));
}

TEST(AlwaysIncludeRuleTest, RejectsRightSchemaWrongEncoding) {
  ChannelInfo ch;
  ch.topic = "/tf";
  ch.schema = "tf2_msgs/msg/TFMessage";
  ch.encoding = "json";
  ch.msg_count = 10;
  EXPECT_FALSE(isAlwaysIncluded(ch));
}

TEST(AlwaysIncludeRuleTest, RejectsRightEncodingWrongSchema) {
  ChannelInfo ch;
  ch.topic = "/sensor/value";
  ch.schema = "std_msgs/Float64";
  ch.encoding = "cdr";
  ch.msg_count = 10;
  EXPECT_FALSE(isAlwaysIncluded(ch));
}

TEST(McapDialogTest, ForceIncludesOnlyWhitelistedNonEmptyChannels) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  // Saved selection predates this feature and omits everything except one
  // ordinary topic -- as if loading an old layout.
  cfg["selected_topics"] = std::vector<std::string>{"/sensor/value2"};

  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  // Pin the fixture: all five channels must be present in the analyzed table,
  // otherwise the negative assertions below would pass vacuously.
  const auto data = nlohmann::json::parse(dialog.widget_data());
  std::set<std::string> row_topics;
  for (const auto& row : data["tableWidget"]["rows"]) {
    row_topics.insert(row[0].get<std::string>());
  }
  EXPECT_EQ(
      row_topics, (std::set<std::string>{"/near_miss_encoding", "/sensor/value2", "/tf", "/tf_static", "transforms"}));

  const auto& selected = dialog.selectedTopics();
  EXPECT_TRUE(selected.count("/tf") > 0);                // whitelisted, has messages -> forced back in
  EXPECT_TRUE(selected.count("transforms") > 0);         // whitelisted (any topic name) -> forced back in
  EXPECT_EQ(selected.count("/tf_static"), 0u);           // whitelisted schema/encoding, zero messages -> NOT forced
  EXPECT_EQ(selected.count("/near_miss_encoding"), 0u);  // right schema, wrong encoding -> NOT forced
  EXPECT_TRUE(selected.count("/sensor/value2") > 0);     // explicitly saved by the user -> stays
}

TEST(McapDialogTest, WhitelistedRowsAreDisabledAndTooltippedInWidgetData) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  const auto data = nlohmann::json::parse(dialog.widget_data());
  const auto& rows = data["tableWidget"]["rows"];

  int tf_row = -1;
  int ordinary_row = -1;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i][0] == "/tf") {
      tf_row = static_cast<int>(i);
    }
    if (rows[i][0] == "/sensor/value2") {
      ordinary_row = static_cast<int>(i);
    }
  }
  ASSERT_NE(tf_row, -1);
  ASSERT_NE(ordinary_row, -1);

  const auto& disabled = data["tableWidget"]["disabled_rows"];
  EXPECT_NE(std::find(disabled.begin(), disabled.end(), tf_row), disabled.end());
  EXPECT_EQ(std::find(disabled.begin(), disabled.end(), ordinary_row), disabled.end());

  ASSERT_TRUE(data["tableWidget"].contains("cell_tooltips"));
  const auto& tooltips = data["tableWidget"]["cell_tooltips"];
  EXPECT_TRUE(tooltips.contains(std::to_string(tf_row) + ",0"));
}

TEST(McapDialogTest, DeselectAllKeepsWhitelistedTopicsSelected) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  EXPECT_TRUE(dialog.onClicked("btnDeselectAll"));

  const auto& selected = dialog.selectedTopics();
  EXPECT_TRUE(selected.count("/tf") > 0);
  EXPECT_TRUE(selected.count("transforms") > 0);
  EXPECT_EQ(selected.count("/sensor/value2"), 0u);
}

TEST(McapDialogTest, HostReportedSelectionOmittingWhitelistIsOverridden) {
  McapDialog dialog;
  nlohmann::json cfg;
  cfg["filepath"] = std::string(MCAP_TEST_DATA_DIR) + "/test_dialog_whitelist.mcap";
  ASSERT_TRUE(dialog.loadConfig(cfg.dump()));

  // Simulate the host reporting a selection that omits /tf and transforms --
  // the dialog must add them back regardless.
  EXPECT_TRUE(dialog.onSelectionChanged("tableWidget", {"/sensor/value2"}));

  const auto& selected = dialog.selectedTopics();
  EXPECT_TRUE(selected.count("/tf") > 0);
  EXPECT_TRUE(selected.count("transforms") > 0);
  EXPECT_TRUE(selected.count("/sensor/value2") > 0);
}

}  // namespace
