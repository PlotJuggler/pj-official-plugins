#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

#include "csv_parser.hpp"

enum class TimeMode { RowNumber, Column, Combined };

inline std::string timeModeToString(TimeMode mode) {
  switch (mode) {
    case TimeMode::RowNumber:
      return "row_number";
    case TimeMode::Column:
      return "column";
    case TimeMode::Combined:
      return "combined";
  }
  return "row_number";
}

inline TimeMode stringToTimeMode(const std::string& s) {
  if (s == "column") {
    return TimeMode::Column;
  }
  if (s == "combined") {
    return TimeMode::Combined;
  }
  return TimeMode::RowNumber;
}

class CsvDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  char delimiter() const {
    return delimiter_;
  }
  const std::string& customTimeFormat() const {
    return custom_format_;
  }
  bool useCustomFormat() const {
    return use_custom_format_;
  }
  const std::vector<PJ::CSV::CombinedColumnPair>& combinedPairs() const {
    return combined_pairs_;
  }

  int timeColumnIndex() const;
  int combinedColumnIndex() const;

  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;

  bool onIndexChanged(std::string_view widget_name, int index) override;
  bool onToggled(std::string_view widget_name, bool checked) override;
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override;
  bool onItemDoubleClicked(std::string_view widget_name, int index) override;
  bool onClicked(std::string_view widget_name) override;
  bool onTextChanged(std::string_view widget_name, std::string_view text) override;
  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

  std::string saveConfig() const override;
  bool loadConfig(std::string_view config_json) override;

 private:
  /// The picked columns by name, empty when none is picked. An empty name falls back to the index, for configs
  /// saved without names.
  struct PickedColumns {
    std::string time;  // "Selected Column"
    int time_index = -1;
    std::string date;  // "Multiple Columns": the date part
    int date_index = -1;
    std::string time_of_day;  // "Multiple Columns": the time part
    int time_of_day_index = -1;
  };
  PickedColumns pickedColumns() const;
  /// Name of column `index` of the current header; empty when out of range.
  std::string columnName(int index) const;

  void analyzeFile() {
    analyzeFile(pickedColumns());
  }
  /// Re-reads the header and preview, then picks `keep` in the new header: a name the header lacks picks nothing.
  void analyzeFile(const PickedColumns& keep);
  // True when the file has a date-only AND a time-only column, so a combined timestamp can be built.
  bool combinableAvailable() const;
  // Sets combined_pairs_/combined_index_ from the two picked indices; clears them when either is invalid.
  void setCombinedColumns(int date_index, int time_index);
  static int delimiterToIndex(char d);
  static char indexToDelimiter(int idx);

  std::string filepath_;
  char delimiter_ = ',';
  // "Detect Automatically" toggle; forced off and locked when the header is ambiguous, so the user picks.
  bool detect_delimiter_ = true;
  bool delimiter_ambiguous_ = false;
  TimeMode time_mode_ = TimeMode::RowNumber;
  int selected_column_index_ = -1;
  int combined_index_ = -1;
  std::string custom_format_;
  bool use_custom_format_ = false;
  bool accept_requested_ = false;
  bool show_help_requested_ = false;
  bool preview_raw_ = false;
  bool has_duplicate_columns_ = false;
  std::string warning_message_;

  std::vector<std::string> column_names_;
  // Parallel to column_names_.
  std::vector<PJ::CSV::ColumnTypeInfo> column_types_;
  // 0 or 1 element: the picked date+time pair.
  std::vector<PJ::CSV::CombinedColumnPair> combined_pairs_;
  std::vector<std::vector<std::string>> preview_rows_;
  std::string raw_preview_;
};
