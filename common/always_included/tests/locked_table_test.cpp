#include <gtest/gtest.h>

#include <pj_always_included/locked_table.hpp>
#include <set>
#include <string>
#include <test_support/widget_table.hpp>
#include <vector>

namespace {

using PJ::AlwaysIncluded::TableRows;
using toolbox_testing::rowOf;
using toolbox_testing::shownSelection;

/// /imu and /odom are ordinary, /tf is locked.
const TableRows kRows = {
    {{"/imu", "sensor_msgs/msg/Imu"}, false},
    {{"/tf", "tf2_msgs/msg/TFMessage"}, true},
    {{"/odom", "nav_msgs/msg/Odometry"}, false},
};

nlohmann::json render(const TableRows& rows, const std::set<std::string>& selection) {
  PJ::WidgetData wd;
  PJ::AlwaysIncluded::tableToWidgets(wd, "topics", rows, selection);
  return toolbox_testing::table(wd.toJson(), "topics");
}

TEST(LockedTable, LockedRowsAreSelectedDisabledAndTooltipped) {
  const auto table = render(kRows, {"/hidden"});
  ASSERT_EQ(table["rows"].size(), 3U);
  EXPECT_EQ(table["rows"][1], nlohmann::json::array({"/tf", "tf2_msgs/msg/TFMessage"}));
  const int tf = rowOf(table, "/tf");
  EXPECT_EQ(table.value("disabled_rows", std::set<int>{}), (std::set<int>{tf}));
  EXPECT_EQ(table["cell_tooltips"].value(std::to_string(tf) + ",0", std::string{}), PJ::AlwaysIncluded::kTooltip);
  EXPECT_FALSE(table["cell_tooltips"].contains(std::to_string(rowOf(table, "/imu")) + ",0"));
  // The selection includes one the filter hides; the table matches rows by name. A locked row is
  // shown selected without being in the selection.
  EXPECT_EQ(shownSelection(table), (std::set<std::string>{"/hidden", "/tf"}));
  EXPECT_EQ(shownSelection(render(kRows, {"/tf"})), (std::set<std::string>{"/tf"}));
}

TEST(LockedTable, SetsThePlaceholderAndTheSelectButtonShortcuts) {
  PJ::WidgetData wd;
  PJ::AlwaysIncluded::tableToWidgets(wd, "topics", {}, {});
  const auto data = nlohmann::json::parse(wd.toJson());
  EXPECT_EQ(data["topics"].value("list_placeholder", std::string{}), "Click to select, Click+Drag to multi-select");
  EXPECT_EQ(data["btnSelectAll"].value("shortcut", std::string{}), "Ctrl+A");
  EXPECT_EQ(data["btnDeselectAll"].value("shortcut", std::string{}), "Ctrl+Shift+A");
}

TEST(LockedTable, SelectAllAddsTheShownUnlockedRowsAndKeepsHiddenOnes) {
  std::set<std::string> selection = {"/hidden"};
  EXPECT_TRUE(PJ::AlwaysIncluded::applySelectButton("btnSelectAll", selection, kRows));
  EXPECT_EQ(selection, (std::set<std::string>{"/hidden", "/imu", "/odom"}));
}

TEST(LockedTable, DeselectAllDropsEverySelectionButLockedRowsStayShown) {
  std::set<std::string> selection = {"/hidden", "/imu", "/tf"};
  EXPECT_TRUE(PJ::AlwaysIncluded::applySelectButton("btnDeselectAll", selection, kRows));
  EXPECT_TRUE(selection.empty());
  EXPECT_EQ(shownSelection(render(kRows, selection)), (std::set<std::string>{"/tf"}));
  EXPECT_FALSE(PJ::AlwaysIncluded::applySelectButton("buttonConnect", selection, kRows));
}

TEST(LockedTable, AReportedSelectionKeepsHiddenOnesAndNeverAddsLockedRows) {
  std::set<std::string> selection = {"/hidden", "/imu"};
  // The host reports only the rows it shows: /odom now, without /tf.
  PJ::AlwaysIncluded::applyReported(selection, {"/odom"}, kRows);
  EXPECT_EQ(selection, (std::set<std::string>{"/hidden", "/odom"}));
  EXPECT_EQ(shownSelection(render(kRows, selection)), (std::set<std::string>{"/hidden", "/odom", "/tf"}));
  PJ::AlwaysIncluded::applyReported(selection, {"/imu", "/tf"}, kRows);
  EXPECT_EQ(selection, (std::set<std::string>{"/hidden", "/imu"}));
}

}  // namespace
