#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class DataExporterDialog : public PJ::DialogPluginTyped {
 public:
  using AddAllCallback = std::function<std::vector<std::string>()>;
  using ExportSingleCallback = std::function<void(bool, const std::string&)>;
  using ExportMultiCallback = std::function<void(bool, const std::string&, const std::string&)>;

  std::string manifest() const override;
  std::string ui_content() const override;
  std::string widget_data() override;

  bool onItemsDropped(std::string_view widget_name, const std::vector<std::string>& items) override;
  bool onClicked(std::string_view widget_name) override;
  bool onItemDeleteRequested(std::string_view widget_name, int index) override;
  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override;
  bool onTextChanged(std::string_view widget_name, std::string_view text) override;
  bool onToggled(std::string_view widget_name, bool checked) override;
  bool onRangeChanged(std::string_view widget_name, int lower, int upper) override;
  bool onFileSelected(std::string_view widget_name, std::string_view path) override;
  bool onFolderSelected(std::string_view widget_name, std::string_view path) override;
  bool onTick() override;
  void onAccepted(std::string_view final_state_json) override;
  void onRejected() override;

  std::string saveConfig() const override;
  bool loadConfig(std::string_view config_json) override;

  void setOnRecomputeRange(std::function<void()> callback);
  void setOnAddAll(AddAllCallback callback);
  void setOnExportSingle(ExportSingleCallback callback);
  void setOnExportMulti(ExportMultiCallback callback);

  void setDataRange(std::optional<std::pair<double, double>> range);
  void setHasStreamingTopics(bool has_streaming);
  void setStatus(std::string status);
  void requestAcceptAfterExport();
  void clearPendingAccept();

  [[nodiscard]] const std::vector<std::string>& topics() const;
  [[nodiscard]] std::pair<double, double> absoluteTimeRange() const;
  [[nodiscard]] int sliderScale() const;

 private:
  static std::string trim(std::string_view text);
  static bool endsWithCaseInsensitive(std::string_view text, std::string_view suffix);
  bool appendTopics(const std::vector<std::string>& topics);
  void invokeRecomputeRange();
  void updateSliderMapping();
  [[nodiscard]] int sliderPosition(double value) const;
  [[nodiscard]] std::vector<int> visibleRows() const;

  std::vector<std::string> topics_;
  std::vector<int> selected_rows_;
  std::string filter_;
  bool relative_time_ = true;
  bool has_streaming_topics_ = false;
  double data_tmin_s_ = 0.0;
  double data_tmax_s_ = 1.0;
  double display_min_s_ = 0.0;
  double display_max_s_ = 1.0;
  double time_offset_ = 0.0;
  double sel_start_s_ = 0.0;
  double sel_end_s_ = 1.0;
  bool export_csv_ = true;
  bool multifile_ = false;
  std::string prefix_;
  std::string last_directory_;
  std::string status_;
  bool pending_accept_ = false;
  int slider_scale_ = 1000;
  int slider_max_ = 1000;
  bool dirty_ = true;

  std::function<void()> on_recompute_range_;
  AddAllCallback on_add_all_;
  ExportSingleCallback on_export_single_;
  ExportMultiCallback on_export_multi_;
};

class DataExporterToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override;
  PJ_borrowed_dialog_t getDialog() override;
  PJ::Status bind(PJ::sdk::ServiceRegistry services) override;
  std::string saveConfig() const override;
  PJ::Status loadConfig(std::string_view config_json) override;

  // getDialog() remains in plugin_entry.cpp because PJ::borrowDialog needs the
  // PJ_DIALOG_PLUGIN-generated vtable specialization in that translation unit.
  void prepareDialog();

  [[nodiscard]] DataExporterDialog& dialog();
  [[nodiscard]] const DataExporterDialog& dialog() const;

 private:
  void wireCallbacks();
  void refreshCatalog();
  [[nodiscard]] std::vector<std::string> allCatalogFields();
  void recomputeRange();
  void exportSingle(bool is_csv, const std::string& path);
  void exportMulti(bool is_csv, const std::string& directory, const std::string& prefix);
  void report(PJ::ToolboxMessageLevel level, const std::string& message);

  DataExporterDialog dialog_;
  std::map<std::string, PJ::sdk::FieldHandle> field_index_;
  std::map<std::string, std::string> field_topic_;
  std::map<std::string, PJ::PrimitiveType> field_type_;
  std::set<std::string> reported_duplicate_paths_;
  bool callbacks_wired_ = false;
};
