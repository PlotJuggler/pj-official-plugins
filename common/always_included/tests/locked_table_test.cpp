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
  // The selection includes one the filter hides; the table matches rows by name.
  EXPECT_EQ(shownSelection(table), (std::set<std::string>{"/hidden", "/tf"}));
}

TEST(LockedTable, SelectionWithLockedAddsTheLockedRows) {
  EXPECT_EQ(PJ::AlwaysIncluded::withLocked({"/imu"}, kRows), (std::set<std::string>{"/imu", "/tf"}));
}

TEST(LockedTable, SelectAllAddsTheShownUnlockedRowsAndKeepsHiddenOnes) {
  std::set<std::string> selection = {"/hidden"};
  PJ::AlwaysIncluded::selectAll(selection, kRows);
  EXPECT_EQ(selection, (std::set<std::string>{"/hidden", "/imu", "/odom"}));
}

TEST(LockedTable, DeselectAllDropsEverythingButLockedRows) {
  std::set<std::string> selection = {"/hidden", "/imu", "/tf"};
  PJ::AlwaysIncluded::deselectAll(selection, kRows);
  EXPECT_EQ(selection, (std::set<std::string>{"/tf"}));
  EXPECT_EQ(shownSelection(render(kRows, selection)), (std::set<std::string>{"/tf"}));
}

TEST(LockedTable, AReportedSelectionKeepsHiddenOnesAndNeverDropsLockedRows) {
  std::set<std::string> selection = {"/hidden", "/imu"};
  // The host reports only the rows it shows: /odom now, without /tf.
  PJ::AlwaysIncluded::applyReported(selection, {"/odom"}, kRows);
  EXPECT_EQ(selection, (std::set<std::string>{"/hidden", "/odom"}));
  EXPECT_EQ(shownSelection(render(kRows, selection)), (std::set<std::string>{"/hidden", "/odom", "/tf"}));
  PJ::AlwaysIncluded::applyReported(selection, {"/imu", "/tf"}, kRows);
  EXPECT_EQ(selection, (std::set<std::string>{"/hidden", "/imu"}));
}

}  // namespace
