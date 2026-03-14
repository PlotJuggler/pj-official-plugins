#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include <nlohmann/json.hpp>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

// Generated from ui/dataload_parquet.ui at configure time
#include "dataload_parquet_ui.hpp"

class ParquetDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  // --- Accessors for ParquetSource ---

  /// Returns the selected time column index, or -1 if using row number.
  int timeColumnIndex() const {
    if (time_mode_ == "column" && selected_column_index_ >= 0) {
      return selected_column_index_;
    }
    return -1;
  }

  bool useCustomDateFormat() const { return use_custom_date_format_; }
  const std::string& customDateFormat() const { return custom_date_format_; }

  void setFilePath(const std::string& filepath) {
    filepath_ = filepath;
    analyzeFile();
  }

  // --- Dialog protocol ---

  std::string manifest() const override {
    return R"({"name":"Parquet File Source","version":"1.0.0"})";
  }

  std::string ui_content() const override { return kDataLoadParquetUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Time axis radios
    wd.setChecked("radioButtonIndex", time_mode_ == "row_number");
    wd.setChecked("radioButtonSelect", time_mode_ == "column");

    // Column list
    wd.setListItems("listWidgetSeries", column_names_);
    wd.setEnabled("listWidgetSeries", time_mode_ == "column");
    if (selected_column_index_ >= 0 &&
        selected_column_index_ < static_cast<int>(column_names_.size())) {
      wd.setSelectedItems("listWidgetSeries",
                          {column_names_[static_cast<size_t>(selected_column_index_)]});
    }

    // Custom date format
    wd.setChecked("checkBoxDateFormat", use_custom_date_format_);
    wd.setText("lineEditDateFormat", custom_date_format_);
    wd.setEnabled("lineEditDateFormat", use_custom_date_format_);

    // OK enabled?
    bool ok = (time_mode_ == "row_number") ||
              (time_mode_ == "column" && selected_column_index_ >= 0);
    wd.setOkEnabled("buttonBox", ok);

    if (accept_requested_) {
      accept_requested_ = false;
      wd.requestAccept();
    }

    return wd.toJson();
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "checkBoxDateFormat") {
      use_custom_date_format_ = checked;
      return true;
    }
    // Radio buttons only fire on checked=true
    if (!checked) return false;
    if (widget_name == "radioButtonIndex") { time_mode_ = "row_number"; return true; }
    if (widget_name == "radioButtonSelect") { time_mode_ = "column"; return true; }
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
      custom_date_format_ = std::string(text);
      return true;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["filepath"] = filepath_;
    cfg["time_mode"] = time_mode_;
    cfg["time_column_index"] = selected_column_index_;
    cfg["use_custom_date_format"] = use_custom_date_format_;
    cfg["custom_date_format"] = custom_date_format_;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) return false;
    filepath_ = cfg.value("filepath", std::string{});
    time_mode_ = cfg.value("time_mode", std::string("column"));
    selected_column_index_ = cfg.value("time_column_index", -1);
    use_custom_date_format_ = cfg.value("use_custom_date_format", false);
    custom_date_format_ = cfg.value("custom_date_format", std::string{});
    if (!filepath_.empty()) analyzeFile();
    return true;
  }

 private:
  void analyzeFile() {
    column_names_.clear();
    if (filepath_.empty()) return;

    auto infile_result = arrow::io::ReadableFile::Open(filepath_);
    if (!infile_result.ok()) return;
    auto infile = *infile_result;

    auto reader_result = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    if (!reader_result.ok()) return;
    auto reader = std::move(*reader_result);

    std::shared_ptr<arrow::Schema> schema;
    auto status = reader->GetSchema(&schema);
    if (!status.ok()) return;

    for (int i = 0; i < schema->num_fields(); i++) {
      column_names_.push_back(schema->field(i)->name());
    }

    // Auto-select timestamp column if none selected yet
    if (selected_column_index_ < 0) {
      selected_column_index_ = findTimestampColumn(schema);
    }
  }

  static int findTimestampColumn(const std::shared_ptr<arrow::Schema>& schema) {
    // Prefer Arrow TIMESTAMP typed columns
    for (int i = 0; i < schema->num_fields(); i++) {
      if (schema->field(i)->type()->id() == arrow::Type::TIMESTAMP) {
        return i;
      }
    }
    // Fallback: match by name (case-insensitive)
    static const std::vector<std::string> kTimestampNames = {
        "timestamp", "time", "t", "ts", "time_stamp", "datetime", "date_time",
        "_timestamp", "_time"};
    for (int i = 0; i < schema->num_fields(); i++) {
      std::string name = schema->field(i)->name();
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      for (const auto& candidate : kTimestampNames) {
        if (name == candidate) return i;
      }
    }
    return -1;
  }

  std::string filepath_;
  std::string time_mode_ = "column";
  int selected_column_index_ = -1;
  bool use_custom_date_format_ = false;
  std::string custom_date_format_;
  bool accept_requested_ = false;

  std::vector<std::string> column_names_;
};

}  // namespace
