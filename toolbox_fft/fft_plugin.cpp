#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "fft_algorithm.hpp"
#include "fft_dialog_ui.hpp"
#include "fft_manifest.hpp"

// ---------------------------------------------------------------------------
// FFT dialog and datastore integration
// ---------------------------------------------------------------------------

namespace {

using PJ::fft::FftResult;
using PJ::fft::WindowFunction;

bool isNumericType(PJ::PrimitiveType type) {
  return type != PJ::PrimitiveType::kBool && type != PJ::PrimitiveType::kString &&
         type != PJ::PrimitiveType::kUnspecified;
}

template <typename T, typename Callback>
bool invokeNumericCallback(const T* values, std::size_t count, Callback& callback) {
  if (values == nullptr) {
    return false;
  }
  callback(values, count);
  return true;
}

template <typename Callback>
bool visitNumericValues(const PJ::sdk::MaterializedSeriesView& series, Callback&& callback) {
  const std::size_t count = series.rowCount();
  switch (series.type()) {
    case PJ::PrimitiveType::kFloat64:
      return invokeNumericCallback(series.valuesAsFloat64(), count, callback);
    case PJ::PrimitiveType::kFloat32:
      return invokeNumericCallback(series.valuesAsFloat32(), count, callback);
    case PJ::PrimitiveType::kInt8:
      return invokeNumericCallback(series.valuesAsInt8(), count, callback);
    case PJ::PrimitiveType::kInt16:
      return invokeNumericCallback(series.valuesAsInt16(), count, callback);
    case PJ::PrimitiveType::kInt32:
      return invokeNumericCallback(series.valuesAsInt32(), count, callback);
    case PJ::PrimitiveType::kInt64:
      return invokeNumericCallback(series.valuesAsInt64(), count, callback);
    case PJ::PrimitiveType::kUint8:
      return invokeNumericCallback(series.valuesAsUint8(), count, callback);
    case PJ::PrimitiveType::kUint16:
      return invokeNumericCallback(series.valuesAsUint16(), count, callback);
    case PJ::PrimitiveType::kUint32:
      return invokeNumericCallback(series.valuesAsUint32(), count, callback);
    case PJ::PrimitiveType::kUint64:
      return invokeNumericCallback(series.valuesAsUint64(), count, callback);
    case PJ::PrimitiveType::kBool:
    case PJ::PrimitiveType::kString:
    case PJ::PrimitiveType::kUnspecified:
      return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// FFTDialog — owns the UI state: preview chart, FFT chart, selected field,
// controls, last FFT result (so "Save" can materialise it into the datastore).
// ---------------------------------------------------------------------------

class FFTDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return kFftManifest;
  }

  std::string ui_content() const override {
    return kFftDialogUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // The inputFrame wraps the label + chart area and is the drop target.
    // Same pattern as the quaternion plugin — a plain QFrame that the
    // DropEventFilter can find without QGraphicsView interference.
    wd.setDropTarget("inputFrame")
        .setChecked("check_dc_removal", remove_dc_)
        .setChecked("radio_all", !range_zoomed_)
        .setChecked("radio_zoomed", range_zoomed_)
        .setItems("window_combo", {"Hann (recommended)", "Rectangular"})
        .setCurrentIndex("window_combo", window_ == WindowFunction::kHann ? 0 : 1)
        .setText("suffix_edit", suffix_)
        .setText("selected_value", selected_fields_.empty() ? "No input selected" : selected_fields_.front())
        .setEnabled("btn_compute", !selected_fields_.empty())
        .setEnabled("btn_save", !last_result_.frequencies_hz.empty())
        .setEnabled("btn_clear", !selected_fields_.empty())
        .setEnabled("suffix_edit", !selected_fields_.empty())
        .setText("status_label", status_msg_);

    // Input preview chart — interactive zoom enabled so the user can select a
    // time range for "Only data in zoomed area" mode (mirrors PJ 3.x behavior).
    wd.setChartZoomEnabled("chart_input");
    wd.setChartPlaceholder("chart_input", "Drag and drop one numeric time series here\n(from the left panel)");
    wd.setChartPlaceholder("chart_fft", "Calculate the spectrum to preview it here");
    if (!input_series_.empty()) {
      wd.setChartSeries("chart_input", input_series_);
    } else {
      wd.clearChart("chart_input");
    }

    // FFT output chart — single series (Hz vs amplitude). Pin it to orange
    // (matplotlib tab10 color 1) so the frequency spectrum is visually
    // distinct from the input preview above, matching PJ 3.x convention.
    if (!last_result_.frequencies_hz.empty()) {
      std::vector<PJ::ChartPoint> pts;
      pts.reserve(last_result_.frequencies_hz.size());
      for (size_t i = 0; i < last_result_.frequencies_hz.size(); ++i) {
        pts.push_back({last_result_.frequencies_hz[i], last_result_.amplitudes[i]});
      }
      const std::string label = selected_fields_.empty() ? std::string{"FFT"} : selected_fields_.front() + suffix_;
      std::vector<PJ::ChartSeries> fft_series;
      fft_series.push_back({label, std::move(pts), "#ff7f0e"});
      wd.setChartSeries("chart_fft", fft_series);
    } else {
      wd.clearChart("chart_fft");
    }
    wd.setChartZoomEnabled("chart_fft");

    return wd.toJson();
  }

  // --- Event handlers -------------------------------------------------------

  bool onItemsDropped(std::string_view widget_name, const std::vector<std::string>& items) override {
    if (widget_name != "inputFrame") {
      return false;
    }
    if (items.empty()) {
      return false;
    }
    if (std::find(available_fields_.begin(), available_fields_.end(), items.front()) == available_fields_.end()) {
      status_msg_ = "The dropped field is unavailable or is not numeric";
      return true;
    }
    // This tool computes one spectrum at a time. A multi-selection drop chooses
    // the first item explicitly instead of previewing several curves while
    // silently computing only one of them.
    const std::vector<std::string> replacement{items.front()};
    if (selected_fields_ == replacement) {
      return false;
    }
    selected_fields_ = replacement;
    saved_selected_.clear();
    clearFftOutput();
    resetZoomRange();
    status_msg_ = items.size() > 1 ? "Selected '" + items.front() + "' (FFT accepts one input at a time)"
                                   : "Selected '" + items.front() + "'";
    invokeRefreshPreview();
    return true;
  }

  bool onChartViewChanged(
      std::string_view name, double x_min, double x_max, double /*y_min*/, double /*y_max*/) override {
    if (name != "chart_input") {
      return false;
    }
    if (!std::isfinite(x_min) || !std::isfinite(x_max) || x_min >= x_max) {
      return false;
    }
    zoom_range_ = std::pair{x_min, x_max};
    invalidateResult("Zoom range changed — calculate again");
    return true;
  }

  bool onToggled(std::string_view name, bool checked) override {
    if (name == "check_dc_removal") {
      if (remove_dc_ == checked) {
        return false;
      }
      remove_dc_ = checked;
      invalidateResult("DC removal changed — calculate again");
      return true;
    }
    if (name == "radio_all" && checked) {
      if (!range_zoomed_) {
        return false;
      }
      range_zoomed_ = false;
      invalidateResult("Input range changed — calculate again");
      invokeRefreshPreview();
      return true;  // range changed → refresh preview
    }
    if (name == "radio_zoomed" && checked) {
      if (range_zoomed_) {
        return false;
      }
      range_zoomed_ = true;
      invalidateResult("Zoom the input chart to choose a time range");
      invokeRefreshPreview();
      return true;
    }
    return false;
  }

  bool onTextChanged(std::string_view name, std::string_view text) override {
    if (name == "suffix_edit") {
      suffix_ = std::string(text);
      return false;
    }
    return false;
  }

  bool onIndexChanged(std::string_view name, int index) override {
    if (name != "window_combo") {
      return false;
    }
    const WindowFunction next = index == 1 ? WindowFunction::kRectangular : WindowFunction::kHann;
    if (window_ == next) {
      return false;
    }
    window_ = next;
    invalidateResult("Window function changed — calculate again");
    return true;
  }

  /// Called periodically by the dialog engine tick timer. Refresh the input
  /// preview so the chart stays up to date while the non-modal dialog is open.
  bool onTick() override {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_preview_refresh_ < std::chrono::milliseconds(500)) {
      return false;
    }
    last_preview_refresh_ = now;
    invokeRefreshPreview();
    return true;  // widget_data changed → host re-reads it
  }

  bool onClicked(std::string_view name) override {
    if (name == "btn_compute") {
      invokeCompute();
      return true;
    }
    if (name == "btn_save") {
      invokeSave();
      return true;
    }
    if (name == "btn_clear") {
      selected_fields_.clear();
      saved_selected_.clear();
      input_series_.clear();
      clearFftOutput();
      resetZoomRange();
      status_msg_ = "Selection cleared";
      return true;
    }
    return false;
  }

  // --- Persistence ----------------------------------------------------------

  std::string saveConfig() const override {
    nlohmann::json j;
    j["remove_dc"] = remove_dc_;
    j["suffix"] = suffix_;
    j["range_zoomed"] = range_zoomed_;
    j["window"] = window_ == WindowFunction::kHann ? "hann" : "rectangular";
    j["selected_fields"] = selected_fields_.empty() ? saved_selected_ : selected_fields_;
    return j.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto j = nlohmann::json::parse(config_json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      return false;
    }

    bool remove_dc = false;
    std::string suffix = "_FFT";
    bool range_zoomed = false;
    WindowFunction window = WindowFunction::kHann;
    std::vector<std::string> saved_selected;
    try {
      remove_dc = j.value("remove_dc", false);
      suffix = j.value("suffix", std::string{"_FFT"});
      range_zoomed = j.value("range_zoomed", false);
      const std::string window_name = j.value("window", std::string{"hann"});
      if (window_name != "hann" && window_name != "rectangular") {
        return false;
      }
      window = window_name == "rectangular" ? WindowFunction::kRectangular : WindowFunction::kHann;

      if (j.contains("selected_fields")) {
        if (!j["selected_fields"].is_array()) {
          return false;
        }
        for (const auto& item : j["selected_fields"]) {
          if (!item.is_string()) {
            return false;
          }
          saved_selected.push_back(item.get<std::string>());
          break;
        }
      }
    } catch (const nlohmann::json::exception&) {
      return false;
    }

    remove_dc_ = remove_dc;
    suffix_ = std::move(suffix);
    range_zoomed_ = range_zoomed;
    window_ = window;
    saved_selected_ = std::move(saved_selected);
    selected_fields_.clear();
    clearFftOutput();
    resetZoomRange();
    return true;
  }

  // --- Public accessors used by FFTToolbox ----------------------------------

  void setAvailableFields(const std::vector<std::string>& fields) {
    const auto previous_selection = selected_fields_;
    available_fields_ = fields;

    // Keep the desired field pending while a layout's data source is absent.
    // A late catalog update can then restore it instead of silently losing the
    // selection during the first empty/partial snapshot.
    std::optional<std::string> desired_field;
    if (!selected_fields_.empty()) {
      desired_field = selected_fields_.front();
    } else if (!saved_selected_.empty()) {
      desired_field = saved_selected_.front();
    }

    selected_fields_.clear();
    if (desired_field &&
        std::find(available_fields_.begin(), available_fields_.end(), *desired_field) != available_fields_.end()) {
      selected_fields_.push_back(*desired_field);
      saved_selected_.clear();
    } else if (desired_field) {
      saved_selected_ = {*desired_field};
    } else {
      saved_selected_.clear();
    }

    if (selected_fields_ != previous_selection) {
      clearFftOutput();
      resetZoomRange();
      status_msg_ = selected_fields_.empty() ? "The selected input is not available yet" : "Input selection restored";
    }
  }

  void setInputPreview(std::vector<PJ::ChartSeries> series) {
    input_series_ = std::move(series);
  }
  void setStatus(const std::string& msg) {
    status_msg_ = msg;
  }
  void setLastResult(FftResult r) {
    last_result_ = std::move(r);
  }
  void clearFftOutput() {
    last_result_ = {};
  }
  void invalidateResult(const std::string& message) {
    if (!last_result_.frequencies_hz.empty()) {
      clearFftOutput();
    }
    status_msg_ = message;
  }

  // Callbacks into the owning FFTToolbox — set once during dialogContext().
  // The dialog holds them as std::function so the host-specific data-plane
  // logic (readSeries, visibleRange, and spectrum export) stays in the
  // toolbox, not in this UI-only class.
  void setOnRefreshPreview(std::function<void()> cb) {
    on_refresh_preview_ = std::move(cb);
  }
  void setOnCompute(std::function<void()> cb) {
    on_compute_ = std::move(cb);
  }
  void setOnSave(std::function<void()> cb) {
    on_save_ = std::move(cb);
  }

  [[nodiscard]] const std::vector<std::string>& selectedFields() const {
    return selected_fields_;
  }
  [[nodiscard]] bool removeDC() const {
    return remove_dc_;
  }
  [[nodiscard]] bool rangeZoomed() const {
    return range_zoomed_;
  }
  [[nodiscard]] const std::optional<std::pair<double, double>>& zoomRange() const {
    return zoom_range_;
  }
  [[nodiscard]] WindowFunction window() const {
    return window_;
  }
  [[nodiscard]] const std::string& suffix() const {
    return suffix_;
  }
  [[nodiscard]] const FftResult& lastResult() const {
    return last_result_;
  }
  [[nodiscard]] bool hasResult() const {
    return !last_result_.frequencies_hz.empty();
  }

 private:
  void invokeRefreshPreview() {
    if (on_refresh_preview_) {
      on_refresh_preview_();
    }
  }
  void invokeCompute() {
    if (on_compute_) {
      on_compute_();
    }
  }
  void invokeSave() {
    if (on_save_) {
      on_save_();
    }
  }
  void resetZoomRange() {
    zoom_range_.reset();
  }

  std::vector<std::string> available_fields_;
  std::vector<std::string> selected_fields_;
  std::vector<std::string> saved_selected_;
  bool remove_dc_ = false;
  bool range_zoomed_ = false;
  WindowFunction window_ = WindowFunction::kHann;
  // Zoom range in seconds relative to t0_common (set by onChartViewChanged).
  // An empty range is distinct from "all data" and cannot overflow when
  // converted back to nanoseconds.
  std::optional<std::pair<double, double>> zoom_range_;
  std::string suffix_ = "_FFT";
  std::string status_msg_;
  std::vector<PJ::ChartSeries> input_series_;
  FftResult last_result_;
  std::chrono::steady_clock::time_point last_preview_refresh_{};

  std::function<void()> on_refresh_preview_;
  std::function<void()> on_compute_;
  std::function<void()> on_save_;
};

// ---------------------------------------------------------------------------
// FFTToolbox — wires the dialog to the datastore: reads fields, computes FFT,
// optionally exports the result as frequency/amplitude columns in a new source.
// ---------------------------------------------------------------------------

class FFTToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    // Wire the dialog's callbacks to this toolbox the first time getDialog
    // is queried. The dialog invokes these on selection/drop/radio/button events
    // so the host-side data plane (readSeries, visibleRange, spectrum export)
    // runs in reaction to user input — not just at dialog-open time.
    if (!callbacks_wired_) {
      dialog_.setOnRefreshPreview([this]() { refreshInputPreview(); });
      dialog_.setOnCompute([this]() {
        runComputation();
        refreshInputPreview();
      });
      dialog_.setOnSave([this]() { saveLastResult(); });
      callbacks_wired_ = true;
    }
    refreshFieldList();
    refreshInputPreview();
    return PJ::borrowDialog(dialog_);
  }

  PJ::Status bind(PJ::sdk::ServiceRegistry services) override {
    auto status = ToolboxPluginBase::bind(services);
    if (!status) {
      return status;
    }
    refreshFieldList();
    return PJ::okStatus();
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected("Invalid FFT toolbox configuration");
    }
    return PJ::okStatus();
  }

  void onDataChanged() override {
    if (dialog_.hasResult()) {
      dialog_.invalidateResult("Input data changed — calculate again");
    }
    refreshFieldList();
    refreshInputPreview();
  }

 private:
  void refreshFieldList() {
    if (!toolboxHostBound()) {
      return;
    }

    auto catalog = toolboxHost().catalogSnapshot();
    if (!catalog) {
      return;
    }

    std::vector<std::string> fields;
    field_index_.clear();

    auto all_topics = catalog->topics();
    auto all_fields = catalog->fields();

    for (size_t ti = 0; ti < all_topics.size(); ++ti) {
      const auto& topic = all_topics[ti];
      auto topic_name = PJ::sdk::toStringView(topic.name);
      for (uint32_t fi = 0; fi < topic.field_count; ++fi) {
        const auto& field = all_fields[topic.first_field + fi];
        if (!isNumericType(PJ::sdk::fromAbiType(field.type))) {
          continue;
        }
        auto field_name = PJ::sdk::toStringView(field.name);
        std::string path = std::string(topic_name) + "/" + std::string(field_name);
        field_index_[path] = field.handle;
        fields.push_back(path);
      }
    }
    std::sort(fields.begin(), fields.end());
    dialog_.setAvailableFields(fields);
  }

  /// Rebuild the input preview chart for each selected field. Time is plotted
  /// in seconds relative to the first sample of the first series.
  /// Stores t0_common_ns_ so readField() can convert the zoom range back to
  /// absolute nanoseconds when "Only data in zoomed area" is active.
  void refreshInputPreview() {
    std::vector<PJ::ChartSeries> series;
    if (!toolboxHostBound()) {
      dialog_.setInputPreview(std::move(series));
      return;
    }

    auto host = toolboxHost();

    int64_t t0_common = 0;
    bool t0_set = false;

    for (const auto& path : dialog_.selectedFields()) {
      auto it = field_index_.find(path);
      if (it == field_index_.end()) {
        continue;
      }

      auto read = host.readSeries(it->second);
      if (!read) {
        continue;
      }
      auto ts_span = read->timestamps();
      const size_t row_count = read->rowCount();
      if (row_count == 0 || ts_span.size() != row_count) {
        continue;
      }

      if (!t0_set) {
        t0_common = ts_span[0];
        t0_common_ns_ = t0_common;
        t0_set = true;
      }

      std::vector<PJ::ChartPoint> pts;
      pts.reserve(row_count);
      const bool visited = visitNumericValues(*read, [&](const auto* values, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
          const long double offset_ns = static_cast<long double>(ts_span[i]) - static_cast<long double>(t0_common);
          const double x = static_cast<double>(offset_ns / 1.0e9L);
          pts.push_back({x, static_cast<double>(values[i])});
        }
      });
      if (!visited) {
        continue;
      }
      // No explicit color — chart_input uses the Qt Charts theme default.
      series.push_back({path, std::move(pts), ""});
    }
    dialog_.setInputPreview(std::move(series));
  }

  /// Read one field's samples in the current range (all/zoomed) into vectors.
  /// Returns false if the field is missing, non-numeric, or has no samples in range.
  bool readField(const std::string& field_path, std::vector<int64_t>& out_ts, std::vector<double>& out_vals) {
    auto host = toolboxHost();
    auto it = field_index_.find(field_path);
    if (it == field_index_.end()) {
      return false;
    }

    auto read = host.readSeries(it->second);
    if (!read) {
      return false;
    }
    auto ts_span = read->timestamps();
    const size_t row_count = read->rowCount();
    if (row_count == 0 || ts_span.size() != row_count) {
      return false;
    }

    int64_t t_min = ts_span[0];
    int64_t t_max = ts_span[row_count - 1];

    if (dialog_.rangeZoomed()) {
      if (!dialog_.zoomRange()) {
        return false;
      }
      // The chart x-axis shows time in seconds relative to t0_common_ns_.
      // Convert the zoom range back to absolute nanoseconds for filtering.
      const auto [zoom_min, zoom_max] = *dialog_.zoomRange();
      const long double range_min = static_cast<long double>(t0_common_ns_) + zoom_min * 1.0e9L;
      const long double range_max = static_cast<long double>(t0_common_ns_) + zoom_max * 1.0e9L;
      if (range_min < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
          range_max > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return false;
      }
      const auto range_min_ns = static_cast<int64_t>(range_min);
      const auto range_max_ns = static_cast<int64_t>(range_max);
      t_min = std::max(t_min, range_min_ns);
      t_max = std::min(t_max, range_max_ns);
    }

    out_ts.clear();
    out_vals.clear();
    out_ts.reserve(row_count);
    out_vals.reserve(row_count);
    const bool visited = visitNumericValues(*read, [&](const auto* values, std::size_t count) {
      for (std::size_t i = 0; i < count; ++i) {
        if (ts_span[i] < t_min || ts_span[i] > t_max) {
          continue;
        }
        out_ts.push_back(ts_span[i]);
        out_vals.push_back(static_cast<double>(values[i]));
      }
    });
    return visited && !out_ts.empty();
  }

  void runComputation() {
    if (!toolboxHostBound()) {
      dialog_.setStatus("Error: toolbox host not bound");
      return;
    }

    const auto& selected = dialog_.selectedFields();
    if (selected.empty()) {
      dialog_.setStatus("No field selected");
      return;
    }
    if (dialog_.rangeZoomed() && !dialog_.zoomRange()) {
      dialog_.clearFftOutput();
      dialog_.setStatus("Zoom the input chart before calculating the selected range");
      return;
    }

    // Compute the spectrum for the tool's single selected field.
    const std::string& field_path = selected.front();
    std::vector<int64_t> ts;
    std::vector<double> vals;
    if (!readField(field_path, ts, vals)) {
      dialog_.clearFftOutput();
      dialog_.setStatus("Error: no data in selected range");
      return;
    }
    if (ts.size() < 8) {
      dialog_.clearFftOutput();
      dialog_.setStatus("Need at least 8 samples (got " + std::to_string(ts.size()) + ")");
      return;
    }

    auto computation = PJ::fft::computeFft(ts.data(), vals.data(), ts.size(), dialog_.removeDC(), dialog_.window());
    if (!computation) {
      dialog_.clearFftOutput();
      dialog_.setStatus("Error: " + computation.message);
      return;
    }

    const size_t bins = computation.result.frequencies_hz.size();
    const double sample_rate_hz = 1.0 / computation.result.sample_period_seconds;
    const double resolution_hz = sample_rate_hz / static_cast<double>(computation.result.sample_count);
    dialog_.setLastResult(std::move(computation.result));
    std::ostringstream status;
    status << "Computed " << bins << " bins from " << dialog_.lastResult().sample_count << " samples · " << std::fixed
           << std::setprecision(3) << sample_rate_hz << " Hz sample rate · " << resolution_hz << " Hz resolution";
    dialog_.setStatus(status.str());
  }

  /// Export the last FFT result as paired frequency_hz and amplitude columns.
  void saveLastResult() {
    if (!toolboxHostBound()) {
      return;
    }

    const auto& result = dialog_.lastResult();
    if (result.frequencies_hz.empty()) {
      dialog_.setStatus("Nothing to save — run Calculate first");
      return;
    }
    if (dialog_.selectedFields().empty()) {
      return;
    }

    auto host = toolboxHost();
    auto source = host.createDataSource("fft_output");
    if (!source) {
      dialog_.setStatus(std::string("createDataSource failed: ") + source.error());
      return;
    }

    const std::string suffix = dialog_.suffix().empty() ? "_FFT" : dialog_.suffix();
    const std::string topic_name = dialog_.selectedFields().front() + suffix;
    auto topic = host.ensureTopic(*source, topic_name);
    if (!topic) {
      dialog_.setStatus(std::string("ensureTopic failed: ") + topic.error());
      return;
    }

    // Write FFT output as (frequency_hz, amplitude) pairs using the generic
    // record API with a synthetic row-index timestamp.
    // The generic record API currently has no ScatterXY primitive, so preserve
    // both axes as explicit columns and label the UI action as an export.
    for (size_t i = 0; i < result.frequencies_hz.size(); ++i) {
      auto ts = PJ::Timestamp{static_cast<int64_t>(i)};
      const PJ::sdk::NamedFieldValue fields[] = {
          {.name = "frequency_hz", .value = result.frequencies_hz[i]},
          {.name = "amplitude", .value = result.amplitudes[i]},
      };
      if (auto status = host.appendRecord(*topic, ts, PJ::Span<const PJ::sdk::NamedFieldValue>(fields)); !status) {
        dialog_.setStatus("Export failed after " + std::to_string(i) + " points: " + std::string(status.error()));
        return;
      }
    }

    if (runtimeHostBound()) {
      runtimeHost().notifyDataChanged();
    }
    dialog_.setStatus(
        "Exported '" + topic_name + "' with frequency_hz and amplitude columns (" +
        std::to_string(result.frequencies_hz.size()) + " rows)");
  }

  FFTDialog dialog_;
  std::map<std::string, PJ::sdk::FieldHandle> field_index_;
  bool callbacks_wired_ = false;
  // First timestamp (ns) of the first series in the last preview refresh.
  // Used to convert chart zoom range (seconds relative to t0) back to absolute ns.
  int64_t t0_common_ns_ = 0;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(FFTToolbox, kFftManifest)
PJ_DIALOG_PLUGIN(FFTDialog, kFftManifest)
