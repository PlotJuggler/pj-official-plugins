#include <gtest/gtest.h>

#include <climits>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "data_exporter.hpp"
#include "exporter_icons.hpp"

namespace {

using Json = nlohmann::json;

Json render(DataExporterDialog& dialog) {
  return Json::parse(dialog.widget_data());
}

TEST(DataExporterDialogTest, RecomputesForEmptyRemoveAndDuplicateOnlyDrop) {
  DataExporterDialog dialog;
  int recompute_count = 0;
  dialog.setOnRecomputeRange([&recompute_count]() { ++recompute_count; });

  EXPECT_TRUE(dialog.onClicked("removeButton"));
  EXPECT_EQ(recompute_count, 1);

  EXPECT_TRUE(dialog.onItemsDropped("tableWidget", {"  alpha/value  "}));
  EXPECT_EQ(recompute_count, 2);
  ASSERT_EQ(dialog.topics(), std::vector<std::string>({"alpha/value"}));

  EXPECT_TRUE(dialog.onItemsDropped("tableWidget", {"alpha/value", " alpha/value "}));
  EXPECT_EQ(recompute_count, 3);
  EXPECT_EQ(dialog.topics(), std::vector<std::string>({"alpha/value"}));
}

TEST(DataExporterDialogTest, ClearLastRowKeepsRangeAndDisablesTimeControls) {
  DataExporterDialog dialog;
  dialog.setDataRange(std::pair{42.0, 47.0});
  dialog.setOnRecomputeRange([&dialog]() { dialog.setDataRange(std::nullopt); });
  ASSERT_TRUE(dialog.onItemsDropped("tableWidget", {"topic/value"}));

  const Json before = render(dialog);
  ASSERT_TRUE(dialog.onClicked("clearButton"));
  const Json after = render(dialog);

  EXPECT_EQ(after["startTime"]["value"], before["startTime"]["value"]);
  EXPECT_EQ(after["endTime"]["value"], before["endTime"]["value"]);
  EXPECT_EQ(after["rangeSlider"]["range_min"], before["rangeSlider"]["range_min"]);
  EXPECT_EQ(after["rangeSlider"]["range_max"], before["rangeSlider"]["range_max"]);
  EXPECT_FALSE(after["startTime"]["enabled"].get<bool>());
  EXPECT_FALSE(after["endTime"]["enabled"].get<bool>());
  EXPECT_FALSE(after["rangeSlider"]["enabled"].get<bool>());
  EXPECT_FALSE(after["saveButton"]["enabled"].get<bool>());
}

TEST(DataExporterDialogTest, UsesVerbatimPj3InlineSvgIcons) {
  DataExporterDialog dialog;
  const Json state = render(dialog);

  EXPECT_EQ(state["clearButton"]["button_icon_svg"].get<std::string>(), kPj3ClearIconSvg);
  EXPECT_EQ(state["saveButton"]["button_icon_svg"].get<std::string>(), kPj3SaveIconSvg);
  EXPECT_FALSE(state["clearButton"].contains("button_icon_name"));
  EXPECT_FALSE(state["saveButton"].contains("button_icon_name"));
}

TEST(DataExporterDialogTest, FilterIsTrimmedCaseInsensitiveAndClearingExplicitlyUnhidesAllRows) {
  DataExporterDialog dialog;
  ASSERT_TRUE(dialog.onItemsDropped("tableWidget", {" Alpha/Value ", "beta/value", "Gamma/value"}));
  ASSERT_TRUE(dialog.onTextChanged("lineEditFilter", "  ALpHa  "));

  Json state = render(dialog);
  EXPECT_EQ(state["tableWidget"]["visible_rows"], Json::array({0}));
  EXPECT_EQ(state["tableWidget"]["rows"], Json::array({{"Alpha/Value"}, {"beta/value"}, {"Gamma/value"}}));

  ASSERT_TRUE(dialog.onTextChanged("lineEditFilter", "   "));
  state = render(dialog);
  EXPECT_EQ(state["tableWidget"]["visible_rows"], Json::array({0, 1, 2}));
}

TEST(DataExporterDialogTest, SelectionSurvivesRerenderAndHiddenRowsRemainInExportSet) {
  DataExporterDialog dialog;
  ASSERT_TRUE(dialog.onItemsDropped("tableWidget", {"Alpha", "Beta", "Gamma"}));
  ASSERT_TRUE(dialog.onSelectionChanged("tableWidget", {"Gamma", "Alpha"}));
  ASSERT_TRUE(dialog.onTextChanged("lineEditFilter", "beta"));

  const Json first = render(dialog);
  const Json second = render(dialog);
  EXPECT_EQ(first["tableWidget"]["selected_rows"], Json::array({2, 0}));
  EXPECT_EQ(second["tableWidget"]["selected_rows"], first["tableWidget"]["selected_rows"]);
  EXPECT_EQ(second["tableWidget"]["visible_rows"], Json::array({1}));
  EXPECT_EQ(dialog.topics(), std::vector<std::string>({"Alpha", "Beta", "Gamma"}));
}

TEST(DataExporterDialogTest, AcceptRequestStaysStickyUntilHostAcknowledgesIt) {
  DataExporterToolbox toolbox;
  DataExporterDialog& dialog = toolbox.dialog();
  (void)render(dialog);
  dialog.requestAcceptAfterExport();

  // JSON-level coverage cannot execute the real PanelEngine teardown path, so
  // assert both modal and non-modal close commands remain sticky until a callback.
  const Json first = render(dialog);
  const Json second = render(dialog);
  EXPECT_TRUE(first.value("__request_accept", false));
  EXPECT_EQ(first.value("__request_close", ""), "export_complete");
  EXPECT_TRUE(second.value("__request_accept", false));
  EXPECT_EQ(second.value("__request_close", ""), "export_complete");
  EXPECT_TRUE(dialog.onTick());

  dialog.onAccepted("{}");
  Json cleared = render(dialog);
  EXPECT_FALSE(cleared.contains("__request_accept"));
  EXPECT_FALSE(cleared.contains("__request_close"));
  EXPECT_FALSE(dialog.onTick());

  dialog.requestAcceptAfterExport();
  ASSERT_TRUE(render(dialog).contains("__request_close"));
  toolbox.prepareDialog();
  cleared = render(dialog);
  EXPECT_FALSE(cleared.contains("__request_accept"));
  EXPECT_FALSE(cleared.contains("__request_close"));
}

TEST(DataExporterDialogTest, SuffixNormalizationMatchesPj3CanonicalSuffixCondition) {
  DataExporterDialog dialog;
  std::vector<std::pair<bool, std::string>> selected;
  dialog.setOnExportSingle([&selected](bool is_csv, const std::string& path) { selected.emplace_back(is_csv, path); });

  ASSERT_TRUE(dialog.onFileSelected("saveButton", "foo"));
  ASSERT_TRUE(dialog.onFileSelected("saveButton", "foo.csv"));
  ASSERT_TRUE(dialog.onFileSelected("saveButton", "foo.CSV"));
  ASSERT_TRUE(dialog.onFileSelected("saveButton", "foo.txt"));
  ASSERT_TRUE(dialog.onToggled("parquetButton", true));
  ASSERT_TRUE(dialog.onFileSelected("saveButton", "bar"));
  ASSERT_TRUE(dialog.onFileSelected("saveButton", "bar.parquet"));
  ASSERT_TRUE(dialog.onFileSelected("saveButton", "bar.pq"));

  ASSERT_EQ(selected.size(), 7U);
  EXPECT_EQ(selected[0], std::pair(true, std::string{"foo.csv"}));
  EXPECT_EQ(selected[1], std::pair(true, std::string{"foo.csv"}));
  EXPECT_EQ(selected[2], std::pair(true, std::string{"foo.CSV"}));
  EXPECT_EQ(selected[3], std::pair(true, std::string{"foo.txt.csv"}));
  EXPECT_EQ(selected[4], std::pair(false, std::string{"bar.parquet"}));
  EXPECT_EQ(selected[5], std::pair(false, std::string{"bar.parquet"}));
  // PJ3 checks only endsWith(".parquet", CaseInsensitive), despite *.pq in the picker filter.
  EXPECT_EQ(selected[6], std::pair(false, std::string{"bar.pq.parquet"}));
}

TEST(DataExporterDialogTest, EmptyTrimmedPrefixFallsBackToPjExport) {
  DataExporterDialog dialog;
  std::string captured_directory;
  std::string captured_prefix;
  dialog.setOnExportMulti(
      [&captured_directory, &captured_prefix](bool, const std::string& directory, const std::string& prefix) {
        captured_directory = directory;
        captured_prefix = prefix;
      });
  EXPECT_FALSE(dialog.onTextChanged("lineEditPrefix", " \t "));
  ASSERT_TRUE(dialog.onFolderSelected("saveButton", "/tmp/exporter"));
  EXPECT_EQ(captured_directory, "/tmp/exporter");
  EXPECT_EQ(captured_prefix, "pj_export");
}

TEST(DataExporterDialogTest, SliderReducesPrecisionUntilScaledSpanFitsInt) {
  DataExporterDialog dialog;
  constexpr double span = 3'000'000.0;
  static_assert(span * 1000.0 > static_cast<double>(INT_MAX));
  static_assert(span * 100.0 <= static_cast<double>(INT_MAX));
  dialog.setDataRange(std::pair{10.0, 10.0 + span});

  const Json state = render(dialog);
  EXPECT_EQ(dialog.sliderScale(), 100);
  EXPECT_EQ(state["rangeSlider"]["range_min"], 0);
  EXPECT_EQ(state["rangeSlider"]["range_max"], 300'000'000);

  ASSERT_TRUE(dialog.onRangeChanged("rangeSlider", 123, 456));
  const auto [absolute_start, absolute_end] = dialog.absoluteTimeRange();
  EXPECT_DOUBLE_EQ(absolute_start, 11.23);
  EXPECT_DOUBLE_EQ(absolute_end, 14.56);
  const Json round_trip = render(dialog);
  EXPECT_EQ(round_trip["rangeSlider"]["range_lower"], 123);
  EXPECT_EQ(round_trip["rangeSlider"]["range_upper"], 456);
}

TEST(DataExporterDialogTest, SliderHandlesNormalPrecisionZeroDurationAndEpochAbsoluteRange) {
  DataExporterDialog dialog;
  dialog.setDataRange(std::pair{10.0, 2'000'010.0});
  EXPECT_EQ(dialog.sliderScale(), 1000);

  dialog.setDataRange(std::pair{99.0, 99.0});
  Json state = render(dialog);
  EXPECT_EQ(state["rangeSlider"]["range_min"], 0);
  EXPECT_EQ(state["rangeSlider"]["range_max"], 1000);
  EXPECT_EQ(state["rangeSlider"]["range_lower"], 0);
  EXPECT_EQ(state["rangeSlider"]["range_upper"], 0);

  ASSERT_TRUE(dialog.onIndexChanged("comboTime", 1));
  dialog.setDataRange(std::pair{1'700'000'000.125, 1'700'000'002.125});
  state = render(dialog);
  EXPECT_EQ(dialog.sliderScale(), 1000);
  EXPECT_EQ(state["rangeSlider"]["range_max"], 2000);
  EXPECT_DOUBLE_EQ(state["startTime"]["value"].get<double>(), 1'700'000'000.125);
  EXPECT_DOUBLE_EQ(state["endTime"]["value"].get<double>(), 1'700'000'002.125);
}

TEST(DataExporterDialogTest, SliderSaturatesAtIntMaxWhenScaleOneStillCannotRepresentSpan) {
  DataExporterDialog dialog;
  constexpr double span = 3'000'000'000.0;
  static_assert(span > static_cast<double>(INT_MAX));
  dialog.setDataRange(std::pair{10.0, 10.0 + span});

  const Json state = render(dialog);
  EXPECT_EQ(dialog.sliderScale(), 1);
  EXPECT_EQ(state["rangeSlider"]["range_max"], INT_MAX);
  EXPECT_EQ(state["rangeSlider"]["range_lower"], 0);
  EXPECT_EQ(state["rangeSlider"]["range_upper"], INT_MAX);
}

TEST(DataExporterDialogTest, UiSpinboxesHaveWideStaticBounds) {
  DataExporterDialog dialog;
  const std::string ui = dialog.ui_content();
  constexpr std::string_view minimum = "<double>-1000000000000000.000000000000000</double>";
  constexpr std::string_view maximum = "<double>1000000000000000.000000000000000</double>";

  const size_t first_minimum = ui.find(minimum);
  const size_t first_maximum = ui.find(maximum);
  ASSERT_NE(first_minimum, std::string::npos);
  ASSERT_NE(first_maximum, std::string::npos);
  EXPECT_NE(ui.find(minimum, first_minimum + minimum.size()), std::string::npos);
  EXPECT_NE(ui.find(maximum, first_maximum + maximum.size()), std::string::npos);
}

TEST(DataExporterDialogTest, SpinboxCommitsClampToDisplayRangeAndKeepEndpointsOrdered) {
  DataExporterDialog dialog;
  dialog.setDataRange(std::pair{10.0, 20.0});

  ASSERT_TRUE(dialog.onValueChanged("startTime", 8.0));
  ASSERT_TRUE(dialog.onValueChanged("endTime", 2.0));
  Json state = render(dialog);
  EXPECT_DOUBLE_EQ(state["startTime"]["value"].get<double>(), 8.0);
  EXPECT_DOUBLE_EQ(state["endTime"]["value"].get<double>(), 8.0);

  dialog.setDataRange(std::pair{10.0, 20.0});
  ASSERT_TRUE(dialog.onValueChanged("startTime", -5.0));
  ASSERT_TRUE(dialog.onValueChanged("endTime", 50.0));
  state = render(dialog);
  EXPECT_DOUBLE_EQ(state["startTime"]["value"].get<double>(), 0.0);
  EXPECT_DOUBLE_EQ(state["endTime"]["value"].get<double>(), 10.0);
}

TEST(DataExporterDialogTest, ConfigContainsOnlyMultifileAndLastDirectory) {
  DataExporterDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({"multifile":true,"last_directory":"/tmp/last","ignored":42})"));
  const Json config = Json::parse(dialog.saveConfig());
  EXPECT_EQ(config, Json({{"multifile", true}, {"last_directory", "/tmp/last"}}));
  EXPECT_TRUE(render(dialog)["checkBoxMultifile"]["checked"].get<bool>());
}

}  // namespace
