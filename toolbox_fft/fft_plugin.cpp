#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <utility>
#include <vector>

#include "fft_dialog_ui.hpp"
#include "fft_manifest.hpp"

extern "C" {
#include <kissfft/kiss_fftr.h>
}

// ---------------------------------------------------------------------------
// FFT math (ported from PJ 3.x ToolboxFFT / fft_editor.cpp)
// ---------------------------------------------------------------------------

namespace {

struct FftResult {
  std::vector<double> frequencies_hz;
  std::vector<double> amplitudes;
};

std::optional<FftResult> computeFFT(const int64_t* timestamps, const double* values, size_t count, bool remove_dc) {
  if (count < 8) {
    return std::nullopt;
  }

  size_t n = count;
  if ((n & 1U) != 0U) {
    --n;  // make even
  }

  double dt_seconds = static_cast<double>(timestamps[n - 1] - timestamps[0]) / (static_cast<double>(n - 1) * 1e9);
  if (dt_seconds <= 0.0) {
    return std::nullopt;
  }

  std::vector<kiss_fft_scalar> input(n);
  double average = 0.0;
  if (remove_dc) {
    for (size_t i = 0; i < n; ++i) {
      average += values[i];
    }
    average /= static_cast<double>(n);
  }

  for (size_t i = 0; i < n; ++i) {
    input[i] = static_cast<kiss_fft_scalar>(values[i] - average);
  }

  std::vector<kiss_fft_cpx> out(n / 2 + 1);
  kiss_fftr_cfg cfg = kiss_fftr_alloc(static_cast<int>(n), 0, nullptr, nullptr);
  if (cfg == nullptr) {
    return std::nullopt;
  }

  kiss_fftr(cfg, input.data(), out.data());
  KISS_FFT_FREE(cfg);

  FftResult res;
  res.frequencies_hz.reserve(n / 2);
  res.amplitudes.reserve(n / 2);

  const double nd = static_cast<double>(n);
  for (size_t i = 0; i < n / 2; ++i) {
    res.frequencies_hz.push_back(static_cast<double>(i) * (1.0 / dt_seconds) / nd);
    res.amplitudes.push_back(std::hypot(static_cast<double>(out[i].r), static_cast<double>(out[i].i)) / nd);
  }

  return res;
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
        .setText("suffix_edit", suffix_)
        .setEnabled("btn_compute", !selected_fields_.empty())
        .setEnabled("btn_save", !last_result_.frequencies_hz.empty())
        .setText("status_label", status_msg_);

    // Input preview chart — interactive zoom enabled so the user can select a
    // time range for "Only data in zoomed area" mode (mirrors PJ 3.x behavior).
    wd.setChartZoomEnabled("chart_input");
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

    return wd.toJson();
  }

  // --- Event handlers -------------------------------------------------------

  bool onItemsDropped(std::string_view widget_name, const std::vector<std::string>& items) override {
    if (widget_name != "inputFrame") {
      return false;
    }
    bool changed = false;
    for (const auto& path : items) {
      if (std::find(selected_fields_.begin(), selected_fields_.end(), path) != selected_fields_.end()) {
        continue;  // already selected
      }
      selected_fields_.push_back(path);
      changed = true;
    }
    if (changed) {
      clearFftOutput();
      resetZoomRange();
      invokeRefreshPreview();
    }
    return changed;
  }

  bool onChartViewChanged(
      std::string_view name, double x_min, double x_max, double /*y_min*/, double /*y_max*/) override {
    if (name != "chart_input") {
      return false;
    }
    zoom_range_min_ = x_min;
    zoom_range_max_ = x_max;
    return false;  // zoom range is only applied at compute time — no UI refresh needed
  }

  bool onToggled(std::string_view name, bool checked) override {
    if (name == "check_dc_removal") {
      remove_dc_ = checked;
      return false;
    }
    if (name == "radio_all" && checked) {
      range_zoomed_ = false;
      invokeRefreshPreview();
      return true;  // range changed → refresh preview
    }
    if (name == "radio_zoomed" && checked) {
      range_zoomed_ = true;
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

  /// Called periodically by the dialog engine tick timer. Refresh the input
  /// preview so the chart stays up to date while the non-modal dialog is open.
  bool onTick() override {
    invokeRefreshPreview();
    return true;  // widget_data changed → host re-reads it
  }

  bool onClicked(std::string_view name) override {
    if (name == "btn_compute") {
      compute_requested_ = true;
      invokeCompute();
      return true;
    }
    if (name == "btn_save") {
      save_requested_ = true;
      invokeSave();
      return true;
    }
    if (name == "btn_clear") {
      selected_fields_.clear();
      clearFftOutput();
      invokeRefreshPreview();
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
    j["selected_fields"] = selected_fields_;
    return j.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto j = nlohmann::json::parse(config_json, nullptr, false);
    if (j.is_discarded()) {
      return false;
    }

    remove_dc_ = j.value("remove_dc", false);
    suffix_ = j.value("suffix", std::string{"_FFT"});
    range_zoomed_ = j.value("range_zoomed", false);

    saved_selected_.clear();
    if (j.contains("selected_fields") && j["selected_fields"].is_array()) {
      for (const auto& item : j["selected_fields"]) {
        if (item.is_string()) {
          saved_selected_.push_back(item.get<std::string>());
        }
      }
    }
    return true;
  }

  // --- Public accessors used by FFTToolbox ----------------------------------

  void setAvailableFields(const std::vector<std::string>& fields) {
    if (fields == available_fields_) {
      return;
    }
    available_fields_ = fields;

    // Reconcile previously-selected fields that may have disappeared and
    // re-attach any saved_selected_ items from config that now exist.
    std::vector<std::string> merged = selected_fields_;
    for (const auto& saved : saved_selected_) {
      if (std::find(merged.begin(), merged.end(), saved) == merged.end()) {
        merged.push_back(saved);
      }
    }
    selected_fields_.clear();
    for (const auto& sel : merged) {
      if (std::find(available_fields_.begin(), available_fields_.end(), sel) != available_fields_.end()) {
        selected_fields_.push_back(sel);
      }
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

  // Callbacks into the owning FFTToolbox — set once during dialogContext().
  // The dialog holds them as std::function so the host-specific data-plane
  // logic (readSeries, visibleRange, register/append ScatterXY) stays in the
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
  [[nodiscard]] double zoomRangeMin() const {
    return zoom_range_min_;
  }
  [[nodiscard]] double zoomRangeMax() const {
    return zoom_range_max_;
  }
  [[nodiscard]] const std::string& suffix() const {
    return suffix_;
  }
  [[nodiscard]] const FftResult& lastResult() const {
    return last_result_;
  }

  [[nodiscard]] bool consumeComputeRequest() {
    const bool req = compute_requested_;
    compute_requested_ = false;
    return req;
  }
  [[nodiscard]] bool consumeSaveRequest() {
    const bool req = save_requested_;
    save_requested_ = false;
    return req;
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
    zoom_range_min_ = std::numeric_limits<double>::lowest();
    zoom_range_max_ = std::numeric_limits<double>::max();
  }

  std::vector<std::string> available_fields_;
  std::vector<std::string> selected_fields_;
  std::vector<std::string> saved_selected_;
  bool remove_dc_ = false;
  bool range_zoomed_ = false;
  // Zoom range in seconds relative to t0_common (set by onChartViewChanged).
  // Initialized to full range so rangeZoomed() with no user zoom = all data.
  double zoom_range_min_ = std::numeric_limits<double>::lowest();
  double zoom_range_max_ = std::numeric_limits<double>::max();
  std::string suffix_ = "_FFT";
  std::string status_msg_;
  std::vector<PJ::ChartSeries> input_series_;
  FftResult last_result_;
  bool compute_requested_ = false;
  bool save_requested_ = false;

  std::function<void()> on_refresh_preview_;
  std::function<void()> on_compute_;
  std::function<void()> on_save_;
};

// ---------------------------------------------------------------------------
// FFTToolbox — wires the dialog to the datastore: reads fields, computes FFT,
// optionally stores the result as a ScatterXY topic in a new data source.
// ---------------------------------------------------------------------------

class FFTToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    // Wire the dialog's callbacks to this toolbox the first time getDialog
    // is queried. The dialog invokes these on selection/drop/radio/button events
    // so the host-side data plane (readSeries, visibleRange, ScatterXY ingest)
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
    dialog_.loadConfig(config_json);
    return PJ::okStatus();
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
      if (read->type() != PJ::PrimitiveType::kFloat64) {
        continue;
      }

      auto ts_span = read->timestamps();
      const size_t row_count = read->rowCount();
      const double* values = read->valuesAsFloat64();
      if (row_count == 0 || ts_span.size() != row_count || values == nullptr) {
        continue;
      }

      int64_t t_min = ts_span[0];
      int64_t t_max = ts_span[row_count - 1];

      if (!t0_set) {
        t0_common = t_min;
        t0_common_ns_ = t_min;
        t0_set = true;
      }

      std::vector<PJ::ChartPoint> pts;
      pts.reserve(row_count);
      for (size_t i = 0; i < row_count; ++i) {
        if (ts_span[i] < t_min || ts_span[i] > t_max) {
          continue;
        }
        const double x = static_cast<double>(ts_span[i] - t0_common) / 1e9;
        pts.push_back({x, values[i]});
      }
      // No explicit color — chart_input uses the Qt Charts theme default.
      series.push_back({path, std::move(pts), ""});
    }
    dialog_.setInputPreview(std::move(series));
  }

  /// Read one field's samples in the current range (all/zoomed) into vectors.
  /// Returns false if the field is missing, not float64, or has no samples in range.
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
    if (read->type() != PJ::PrimitiveType::kFloat64) {
      return false;
    }

    auto ts_span = read->timestamps();
    const size_t row_count = read->rowCount();
    const double* values = read->valuesAsFloat64();
    if (row_count == 0 || ts_span.size() != row_count || values == nullptr) {
      return false;
    }

    int64_t t_min = ts_span[0];
    int64_t t_max = ts_span[row_count - 1];

    if (dialog_.rangeZoomed()) {
      // The chart x-axis shows time in seconds relative to t0_common_ns_.
      // Convert the zoom range back to absolute nanoseconds for filtering.
      auto range_min_ns = t0_common_ns_ + static_cast<int64_t>(dialog_.zoomRangeMin() * 1e9);
      auto range_max_ns = t0_common_ns_ + static_cast<int64_t>(dialog_.zoomRangeMax() * 1e9);
      t_min = std::max(t_min, range_min_ns);
      t_max = std::min(t_max, range_max_ns);
    }

    out_ts.clear();
    out_vals.clear();
    out_ts.reserve(row_count);
    out_vals.reserve(row_count);
    for (size_t i = 0; i < row_count; ++i) {
      if (ts_span[i] < t_min || ts_span[i] > t_max) {
        continue;
      }
      out_ts.push_back(ts_span[i]);
      out_vals.push_back(values[i]);
    }
    return !out_ts.empty();
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

    // Compute FFT on the first selected field (preview use-case). Save-curve
    // persists this result to the datastore as a ScatterXY topic.
    const std::string& field_path = selected.front();
    std::vector<int64_t> ts;
    std::vector<double> vals;
    if (!readField(field_path, ts, vals)) {
      dialog_.setStatus("Error: no data in selected range");
      return;
    }
    if (ts.size() < 8) {
      dialog_.setStatus("Need at least 8 samples (got " + std::to_string(ts.size()) + ")");
      return;
    }

    auto result = computeFFT(ts.data(), vals.data(), ts.size(), dialog_.removeDC());
    if (!result) {
      dialog_.setStatus("FFT failed — timestamps must be monotonically increasing");
      return;
    }

    const size_t bins = result->frequencies_hz.size();
    dialog_.setLastResult(std::move(*result));
    dialog_.setStatus("Computed " + std::to_string(bins) + " bins for '" + field_path + "'");
  }

  /// Persist the last FFT result as a ScatterXY topic (Hz vs amplitude).
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

    const std::string topic_name = dialog_.selectedFields().front() + dialog_.suffix();
    auto topic = host.ensureTopic(*source, topic_name);
    if (!topic) {
      dialog_.setStatus(std::string("ensureTopic failed: ") + topic.error());
      return;
    }

    // Write FFT output as (frequency_hz, amplitude) pairs using the generic
    // record API with a synthetic row-index timestamp.
    // TODO: replace with registerScatterXYSeries/appendScatterXY when the
    // ScatterXY API is merged (see scatter-xy design report).
    for (size_t i = 0; i < result.frequencies_hz.size(); ++i) {
      auto ts = PJ::Timestamp{static_cast<int64_t>(i)};
      const PJ::sdk::NamedFieldValue fields[] = {
          {.name = "frequency_hz", .value = result.frequencies_hz[i]},
          {.name = "amplitude", .value = result.amplitudes[i]},
      };
      (void)host.appendRecord(*topic, ts, PJ::Span<const PJ::sdk::NamedFieldValue>(fields));
    }

    if (runtimeHostBound()) {
      runtimeHost().notifyDataChanged();
    }
    dialog_.setStatus("Saved '" + topic_name + "' (" + std::to_string(result.frequencies_hz.size()) + " points)");
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
PJ_DIALOG_PLUGIN(FFTDialog)
