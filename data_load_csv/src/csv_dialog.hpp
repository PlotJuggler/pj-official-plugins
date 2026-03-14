#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "csv_parser.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Generated from ui/dataload_csv.ui at configure time
#include "dataload_csv_ui.hpp"

class CsvDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  // --- Accessors for CsvSource ---

  char delimiter() const { return delimiter_; }

  int timeColumnIndex() const {
    if (time_mode_ == "column" && selected_column_index_ >= 0) {
      return selected_column_index_;
    }
    return -1;
  }

  int combinedColumnIndex() const {
    if (time_mode_ == "combined") return combined_index_;
    return -1;
  }

  const std::string& customTimeFormat() const { return custom_format_; }
  bool useCustomFormat() const { return use_custom_format_; }

  const std::vector<PJ::CSV::CombinedColumnPair>& combinedPairs() const {
    return combined_pairs_;
  }

  void setFilePath(const std::string& filepath) {
    filepath_ = filepath;
    analyzeFile();
  }

  // --- Dialog protocol ---

  std::string manifest() const override {
    return R"({"name":"CSV File Source","version":"1.0.0"})";
  }

  std::string ui_content() const override { return kDataLoadCsvUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Delimiter
    std::vector<std::string> delimiters = {
        "\",\" (comma)", "\";\" (semicolon)", "\" \" (space)", "\"\\t\" (tab)"};
    wd.setItems("comboBox", delimiters);
    wd.setCurrentIndex("comboBox", delimiterToIndex(delimiter_));

    // Time axis radios
    wd.setChecked("radioButtonIndex", time_mode_ == "row_number");
    wd.setChecked("radioButtonSelect", time_mode_ == "column");
    wd.setChecked("radioButtonDateTimeColumns", time_mode_ == "combined");

    // Column list
    wd.setListItems("listWidgetSeries", column_names_);
    wd.setEnabled("listWidgetSeries", time_mode_ == "column");
    if (selected_column_index_ >= 0 &&
        selected_column_index_ < static_cast<int>(column_names_.size())) {
      wd.setSelectedItems("listWidgetSeries",
                          {column_names_[static_cast<size_t>(selected_column_index_)]});
    }

    // Combined pairs
    std::vector<std::string> combo_items;
    for (const auto& p : combined_pairs_) combo_items.push_back(p.virtual_name);
    wd.setItems("combinedCombo", combo_items);
    wd.setEnabled("combinedCombo", time_mode_ == "combined");
    wd.setVisible("radioButtonDateTimeColumns", !combined_pairs_.empty());
    wd.setVisible("combinedCombo", !combined_pairs_.empty());

    // Timestamp format
    wd.setChecked("radioAutoTime", !use_custom_format_);
    wd.setChecked("radioCustomTime", use_custom_format_);
    wd.setText("lineEditDateFormat", custom_format_);
    wd.setEnabled("lineEditDateFormat", use_custom_format_);

    // Preview table
    wd.setTableHeaders("tableView", column_names_);
    wd.setTableRows("tableView", preview_rows_);

    // OK enabled?
    bool ok = (time_mode_ == "row_number") ||
              (time_mode_ == "column" && selected_column_index_ >= 0) ||
              (time_mode_ == "combined" && combined_index_ >= 0);
    wd.setOkEnabled("buttonBox", ok);

    if (accept_requested_) {
      accept_requested_ = false;
      wd.requestAccept();
    }

    return wd.toJson();
  }

  bool onIndexChanged(std::string_view widget_name, int index) override {
    if (widget_name == "comboBox") {
      delimiter_ = indexToDelimiter(index);
      analyzeFile();
      return true;
    }
    if (widget_name == "combinedCombo") {
      combined_index_ = index;
      return true;
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (!checked) return false;
    if (widget_name == "radioButtonIndex") { time_mode_ = "row_number"; return true; }
    if (widget_name == "radioButtonSelect") { time_mode_ = "column"; return true; }
    if (widget_name == "radioButtonDateTimeColumns") { time_mode_ = "combined"; return true; }
    if (widget_name == "radioAutoTime") { use_custom_format_ = false; return true; }
    if (widget_name == "radioCustomTime") { use_custom_format_ = true; return true; }
    return false;
  }

  bool onSelectionChanged(std::string_view widget_name,
                          const std::vector<std::string>& selected) override {
    if (widget_name == "listWidgetSeries" && !selected.empty()) {
      for (int i = 0; i < static_cast<int>(column_names_.size()); i++) {
        if (column_names_[static_cast<size_t>(i)] == selected[0]) {
          selected_column_index_ = i;
          return true;
        }
      }
    }
    return false;
  }

  bool onItemDoubleClicked(std::string_view widget_name, int index) override {
    if (widget_name == "listWidgetSeries" && time_mode_ == "column" &&
        index >= 0 && index < static_cast<int>(column_names_.size())) {
      selected_column_index_ = index;
      accept_requested_ = true;
      return true;
    }
    return false;
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "lineEditDateFormat") {
      custom_format_ = std::string(text);
      return true;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["filepath"] = filepath_;
    cfg["delimiter"] = std::string(1, delimiter_);
    cfg["time_mode"] = time_mode_;
    cfg["time_column_index"] = selected_column_index_;
    cfg["combined_column_index"] = combined_index_;
    cfg["custom_time_format"] = custom_format_;
    cfg["use_custom_format"] = use_custom_format_;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) return false;
    filepath_ = cfg.value("filepath", std::string{});
    auto d = cfg.value("delimiter", std::string(","));
    delimiter_ = d.empty() ? ',' : d[0];
    time_mode_ = cfg.value("time_mode", std::string("row_number"));
    selected_column_index_ = cfg.value("time_column_index", -1);
    combined_index_ = cfg.value("combined_column_index", -1);
    custom_format_ = cfg.value("custom_time_format", std::string{});
    use_custom_format_ = cfg.value("use_custom_format", false);
    if (!filepath_.empty()) analyzeFile();
    return true;
  }

 private:
  void analyzeFile() {
    column_names_.clear();
    combined_pairs_.clear();
    preview_rows_.clear();

    if (filepath_.empty()) return;
    std::ifstream file(filepath_);
    if (!file.is_open()) return;

    // Read header
    std::string header_line;
    if (!std::getline(file, header_line)) return;
    if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();

    if (delimiter_ == '\0') delimiter_ = PJ::CSV::DetectDelimiter(header_line);

    column_names_ = PJ::CSV::ParseHeaderLine(header_line, delimiter_);

    // Build preview rows: first 100 data lines (like the original)
    std::string line;
    std::vector<std::string> parts;
    std::vector<PJ::CSV::ColumnTypeInfo> first_row_types;
    int count = 0;
    while (std::getline(file, line) && count < 100) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      PJ::CSV::SplitLine(line, delimiter_, parts);
      preview_rows_.push_back(parts);

      // Detect types from first row
      if (count == 0) {
        first_row_types.resize(column_names_.size());
        for (size_t i = 0; i < parts.size() && i < first_row_types.size(); i++) {
          if (!parts[i].empty()) first_row_types[i] = PJ::CSV::DetectColumnType(parts[i]);
        }
        combined_pairs_ =
            PJ::CSV::DetectCombinedDateTimeColumns(column_names_, first_row_types);
      }
      count++;
    }
  }

  static int delimiterToIndex(char d) {
    switch (d) {
      case ',': return 0;
      case ';': return 1;
      case ' ': return 2;
      case '\t': return 3;
      default: return 0;
    }
  }

  static char indexToDelimiter(int idx) {
    switch (idx) {
      case 0: return ',';
      case 1: return ';';
      case 2: return ' ';
      case 3: return '\t';
      default: return ',';
    }
  }

  std::string filepath_;
  char delimiter_ = ',';
  std::string time_mode_ = "row_number";
  int selected_column_index_ = -1;
  int combined_index_ = -1;
  std::string custom_format_;
  bool use_custom_format_ = false;
  bool accept_requested_ = false;

  std::vector<std::string> column_names_;
  std::vector<PJ::CSV::CombinedColumnPair> combined_pairs_;
  std::vector<std::vector<std::string>> preview_rows_;
};

}  // namespace
