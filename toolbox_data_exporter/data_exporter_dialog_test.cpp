#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "data_exporter.hpp"

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

TEST(DataExporterDialogTest, ClearLastRowKeepsRangeAndDisablesExportControls) {
  DataExporterDialog dialog;
  dialog.setDataRange(std::pair{42.0, 47.0});
  dialog.setOnRecomputeRange([&dialog]() { dialog.setDataRange(std::nullopt); });
  ASSERT_TRUE(dialog.onItemsDropped("tableWidget", {"topic/value"}));

  const Json before = render(dialog);
  ASSERT_TRUE(dialog.onClicked("clearButton"));
  const Json after = render(dialog);

  EXPECT_EQ(after["rangeSlider"]["range_min"], before["rangeSlider"]["range_min"]);
  EXPECT_EQ(after["rangeSlider"]["range_max"], before["rangeSlider"]["range_max"]);
  EXPECT_EQ(after["rangeSlider"]["range_time_min_ns"], before["rangeSlider"]["range_time_min_ns"]);
  EXPECT_EQ(after["rangeSlider"]["range_time_max_ns"], before["rangeSlider"]["range_time_max_ns"]);
  EXPECT_FALSE(after.contains("startTime"));
  EXPECT_FALSE(after.contains("endTime"));
  EXPECT_FALSE(after["rangeSlider"]["enabled"].get<bool>());
  EXPECT_FALSE(after["saveButton"]["enabled"].get<bool>());
}

TEST(DataExporterDialogTest, ExportAndClearButtonsHaveNoIcons) {
  DataExporterDialog dialog;
  const Json state = render(dialog);

  EXPECT_FALSE(state["saveButton"].contains("button_icon_svg"));
  EXPECT_FALSE(state["saveButton"].contains("button_icon_name"));
  EXPECT_FALSE(state.contains("clearButton"));
}

TEST(DataExporterDialogTest, EmitsTablePlaceholderAndRowDeletableFlag) {
  DataExporterDialog dialog;
  const Json state = render(dialog);

  EXPECT_EQ(state["tableWidget"]["list_placeholder"], "Drag and Drop series from the left panel");
  EXPECT_TRUE(state["tableWidget"]["list_deletable"].get<bool>());
}

TEST(DataExporterDialogTest, RowDeleteErasesRequestedTopicClearsSelectionAndRecomputes) {
  DataExporterDialog dialog;
  int recompute_count = 0;
  dialog.setOnRecomputeRange([&recompute_count]() { ++recompute_count; });
  ASSERT_TRUE(dialog.onItemsDropped("tableWidget", {"Alpha", "Beta", "Gamma"}));
  ASSERT_TRUE(dialog.onSelectionChanged("tableWidget", {"Beta"}));
  ASSERT_EQ(recompute_count, 1);

  ASSERT_TRUE(dialog.onItemDeleteRequested("tableWidget", 1));

  EXPECT_EQ(recompute_count, 2);
  EXPECT_EQ(dialog.topics(), std::vector<std::string>({"Alpha", "Gamma"}));
  const Json state = render(dialog);
  EXPECT_EQ(state["tableWidget"]["rows"], Json::array({{"Alpha"}, {"Gamma"}}));
  EXPECT_EQ(state["tableWidget"]["selected_rows"], Json::array());
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
  EXPECT_EQ(state["rangeSlider"]["range_time_min_ns"], "10000000000");
  EXPECT_EQ(state["rangeSlider"]["range_time_max_ns"], "3000010000000000");

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
  EXPECT_EQ(state["rangeSlider"]["range_time_min_ns"], "99000000000");
  EXPECT_EQ(state["rangeSlider"]["range_time_max_ns"], "99000000000");

  ASSERT_TRUE(dialog.onToggled("radioAbsolute", true));
  constexpr double kEpochStart = 1'700'000'000.125;
  constexpr double kEpochEnd = 1'700'000'002.125;
  dialog.setDataRange(std::pair{kEpochStart, kEpochEnd});
  state = render(dialog);
  EXPECT_EQ(dialog.sliderScale(), 1000);
  EXPECT_EQ(state["rangeSlider"]["range_max"], 2000);
  EXPECT_FALSE(state["radioRelative"]["checked"].get<bool>());
  EXPECT_TRUE(state["radioAbsolute"]["checked"].get<bool>());
  const auto epoch_start_ns = static_cast<std::int64_t>(kEpochStart * 1'000'000'000.0);
  const auto epoch_end_ns = static_cast<std::int64_t>(kEpochEnd * 1'000'000'000.0);
  EXPECT_EQ(state["rangeSlider"]["range_time_min_ns"], std::to_string(epoch_start_ns));
  EXPECT_EQ(state["rangeSlider"]["range_time_max_ns"], std::to_string(epoch_end_ns));
  EXPECT_FALSE(state.contains("startTime"));
  EXPECT_FALSE(state.contains("endTime"));
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

TEST(DataExporterDialogTest, UiUsesTimeRadiosRemovesSpinboxesAndLabelsClearButton) {
  DataExporterDialog dialog;
  const std::string ui = dialog.ui_content();

  EXPECT_NE(ui.find("name=\"radioRelative\""), std::string::npos);
  EXPECT_NE(ui.find("name=\"radioAbsolute\""), std::string::npos);
  EXPECT_EQ(ui.find("name=\"comboTime\""), std::string::npos);
  EXPECT_EQ(ui.find("name=\"startTime\""), std::string::npos);
  EXPECT_EQ(ui.find("name=\"endTime\""), std::string::npos);

  const size_t clear_button = ui.find("name=\"clearButton\"");
  ASSERT_NE(clear_button, std::string::npos);
  const size_t clear_button_end = ui.find("</widget>", clear_button);
  ASSERT_NE(clear_button_end, std::string::npos);
  const size_t clear_text = ui.find("<string>Clear</string>", clear_button);
  ASSERT_NE(clear_text, std::string::npos);
  EXPECT_LT(clear_text, clear_button_end);
}

TEST(DataExporterDialogTest, TimeModeRadiosRecomputeRangeAndPreserveAbsoluteSpan) {
  DataExporterDialog dialog;
  dialog.setDataRange(std::pair{10.0, 20.0});
  int recompute_count = 0;
  dialog.setOnRecomputeRange([&dialog, &recompute_count]() {
    ++recompute_count;
    dialog.setDataRange(std::pair{10.0, 20.0});
  });

  Json state = render(dialog);
  EXPECT_TRUE(state["radioRelative"]["checked"].get<bool>());
  EXPECT_FALSE(state["radioAbsolute"]["checked"].get<bool>());
  EXPECT_FALSE(state.contains("comboTime"));

  ASSERT_TRUE(dialog.onToggled("radioRelative", false));
  EXPECT_EQ(recompute_count, 1);
  ASSERT_TRUE(dialog.onToggled("radioAbsolute", true));
  EXPECT_EQ(recompute_count, 1);
  state = render(dialog);
  EXPECT_FALSE(state["radioRelative"]["checked"].get<bool>());
  EXPECT_TRUE(state["radioAbsolute"]["checked"].get<bool>());
  EXPECT_EQ(state["rangeSlider"]["range_time_min_ns"], "10000000000");
  EXPECT_EQ(state["rangeSlider"]["range_time_max_ns"], "20000000000");
  EXPECT_EQ(dialog.absoluteTimeRange(), std::pair(10.0, 20.0));

  ASSERT_TRUE(dialog.onToggled("radioAbsolute", false));
  EXPECT_EQ(recompute_count, 2);
  ASSERT_TRUE(dialog.onToggled("radioRelative", true));
  EXPECT_EQ(recompute_count, 2);
  state = render(dialog);
  EXPECT_TRUE(state["radioRelative"]["checked"].get<bool>());
  EXPECT_FALSE(state["radioAbsolute"]["checked"].get<bool>());
  EXPECT_EQ(state["rangeSlider"]["range_time_min_ns"], "10000000000");
  EXPECT_EQ(state["rangeSlider"]["range_time_max_ns"], "20000000000");
  EXPECT_EQ(dialog.absoluteTimeRange(), std::pair(10.0, 20.0));
}

TEST(DataExporterDialogTest, ConfigContainsOnlyMultifileAndLastDirectory) {
  DataExporterDialog dialog;
  ASSERT_TRUE(dialog.loadConfig(R"({"multifile":true,"last_directory":"/tmp/last","ignored":42})"));
  const Json config = Json::parse(dialog.saveConfig());
  EXPECT_EQ(config, Json({{"multifile", true}, {"last_directory", "/tmp/last"}}));
  EXPECT_TRUE(render(dialog)["checkBoxMultifile"]["checked"].get<bool>());
}

}  // namespace
