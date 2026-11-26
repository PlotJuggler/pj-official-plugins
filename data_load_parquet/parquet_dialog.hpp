#pragma once

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

#include "parquet_helpers.hpp"

namespace {

// Generated at configure time
#include "dataload_parquet_ui.hpp"
#include "parquet_manifest.hpp"

class ParquetDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  /// Returns the selected time column index, or -1 if using row number.
  int timeColumnIndex() const {
    if (time_mode_ == "column" && selected_column_index_ >= 0) {
      return selected_column_index_;
    }
    return -1;
  }

  bool useCustomDateFormat() const {
    return use_custom_date_format_;
  }
  const std::string& customDateFormat() const {
    return custom_date_format_;
  }

  std::string manifest() const override {
    return kParquetManifest;
  }

  std::string ui_content() const override {
    return kDataLoadParquetUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    wd.setChecked("radioButtonIndex", time_mode_ == "row_number");
    wd.setChecked("radioButtonSelect", time_mode_ == "column");

    wd.setListItems("listWidgetSeries", column_names_);
    wd.setEnabled("listWidgetSeries", time_mode_ == "column");
    if (selected_column_index_ >= 0 && selected_column_index_ < static_cast<int>(column_names_.size())) {
      wd.setSelectedItems("listWidgetSeries", {column_names_[static_cast<size_t>(selected_column_index_)]});
    }

    wd.setChecked("checkBoxDateFormat", use_custom_date_format_);
    wd.setText("lineEditDateFormat", custom_date_format_);
    wd.setEnabled("lineEditDateFormat", use_custom_date_format_);

    bool ok = (time_mode_ == "row_number") || (time_mode_ == "column" && selected_column_index_ >= 0);
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
    if (!checked) {
      return false;
    }
    if (widget_name == "radioButtonIndex") {
      time_mode_ = "row_number";
      return true;
    }
    if (widget_name == "radioButtonSelect") {
      time_mode_ = "column";
      return true;
    }
    return false;
  }

  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override {
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
    if (widget_name == "listWidgetSeries" && time_mode_ == "column" && index >= 0 &&
        index < static_cast<int>(column_names_.size())) {
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
    // The column is restored by name: PlotJuggler pre-fills the next file's dialog with this config. The index is
    // also written so a plugin build that reads only the index can still load the config.
    cfg["time_column_name"] =
        selected_column_index_ >= 0 && selected_column_index_ < static_cast<int>(column_names_.size())
            ? column_names_[static_cast<size_t>(selected_column_index_)]
            : std::string{};
    cfg["time_column_index"] = selected_column_index_;
    cfg["use_custom_date_format"] = use_custom_date_format_;
    cfg["custom_date_format"] = custom_date_format_;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    filepath_ = cfg.value("filepath", std::string{});
    time_mode_ = cfg.value("time_mode", std::string("column"));
    use_custom_date_format_ = cfg.value("use_custom_date_format", false);
    custom_date_format_ = cfg.value("custom_date_format", std::string{});
    analyzeFile(cfg.value("time_column_name", std::string{}), cfg.value("time_column_index", -1));
    return true;
  }

 private:
  /// Reads the schema and picks the time column named `time_column` in it. The schema may not be the one the name
  /// was saved for: a name it lacks picks the detected timestamp column. An empty name falls back to
  /// `time_column_index` (configs that carry only the index).
  void analyzeFile(const std::string& time_column, int time_column_index) {
    column_names_.clear();
    selected_column_index_ = -1;
    if (filepath_.empty()) {
      return;
    }

    auto infile_result = arrow::io::ReadableFile::Open(filepath_);
    if (!infile_result.ok()) {
      return;
    }
    auto infile = *infile_result;

    auto reader_result = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    if (!reader_result.ok()) {
      return;
    }
    auto reader = std::move(*reader_result);

    std::shared_ptr<arrow::Schema> schema;
    auto status = reader->GetSchema(&schema);
    if (!status.ok()) {
      return;
    }

    for (int i = 0; i < schema->num_fields(); i++) {
      column_names_.push_back(schema->field(i)->name());
    }

    if (!time_column.empty()) {
      const auto it = std::find(column_names_.begin(), column_names_.end(), time_column);
      selected_column_index_ = it == column_names_.end() ? -1 : static_cast<int>(it - column_names_.begin());
    } else if (time_column_index < schema->num_fields()) {
      selected_column_index_ = time_column_index;
    }
    if (selected_column_index_ < 0) {
      selected_column_index_ = PJ::ParquetHelpers::findTimestampColumn(schema);
    }
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
