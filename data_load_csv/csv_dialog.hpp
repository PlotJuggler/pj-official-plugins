#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "csv_parser.hpp"

#include <string>
#include <vector>

class CsvDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  // --- Accessors for CsvSource ---

  char delimiter() const { return delimiter_; }
  const std::string& customTimeFormat() const { return custom_format_; }
  bool useCustomFormat() const { return use_custom_format_; }
  const std::vector<PJ::CSV::CombinedColumnPair>& combinedPairs() const { return combined_pairs_; }

  int timeColumnIndex() const;
  int combinedColumnIndex() const;
  void setFilePath(const std::string& filepath);

  // --- Dialog protocol ---

  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;

  bool onIndexChanged(std::string_view widget_name, int index) override;
  bool onToggled(std::string_view widget_name, bool checked) override;
  bool onSelectionChanged(std::string_view widget_name,
                          const std::vector<std::string>& selected) override;
  bool onItemDoubleClicked(std::string_view widget_name, int index) override;
  bool onClicked(std::string_view widget_name) override;
  bool onTextChanged(std::string_view widget_name, std::string_view text) override;
  void onAccepted(std::string_view json) override;
  void onRejected() override {}

  std::string saveConfig() const override;
  bool loadConfig(std::string_view config_json) override;

 private:
  void analyzeFile();
  static std::string computeFormatPreview(const std::string& fmt);
  static int delimiterToIndex(char d);
  static char indexToDelimiter(int idx);

  std::string filepath_;
  char delimiter_ = ',';
  std::string time_mode_ = "row_number";
  int selected_column_index_ = -1;
  int combined_index_ = -1;
  std::string custom_format_;
  bool use_custom_format_ = false;
  bool accept_requested_ = false;
  bool show_help_requested_ = false;

  std::vector<std::string> column_names_;
  std::vector<std::string> column_history_;
  std::vector<PJ::CSV::CombinedColumnPair> combined_pairs_;
  std::vector<std::vector<std::string>> preview_rows_;
};
