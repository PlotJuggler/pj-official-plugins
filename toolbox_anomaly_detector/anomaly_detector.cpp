// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// Anomaly Detector toolbox (demo), styled after the host Filter Editor: a Preview
// chart on top, then Source curve | Function | Lua editor. Picking a predefined
// Function fills the editor with its Lua (targeting the selected source); the
// "-- No function --" entry leaves a blank template to write your own. Apply runs
// the script via the shared `anomaly_core` engine, which delegates to the Luau
// marker engine in pj_scripting_core — the SAME engine PlotJuggler uses for filters
// and the headless anomaly_runner CLI uses — and publishes the emitted set
// as a PlotMarkers object via the toolbox object-write surface (per-series under the
// selected source, or dataset-global when "Global marker" is ticked). The status
// shows "Done: N marker(s)" or "Error".
//
// It is a Toolbox because only that plugin family can read timeseries
// (catalogSnapshot + readSeries) and write objects. The detection logic itself is
// GUI-free and lives in anomaly_core; this file is only the host + dialog wiring.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <pj_base/builtin/plot_markers.hpp>
#include <pj_base/builtin/plot_markers_codec.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_base/span.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "anomaly_detector_dialog_ui.hpp"
#include "anomaly_detector_manifest.hpp"
#include "core/anomaly_core.hpp"

namespace {

// Opaque metadata tagging the object topic as a marker set (same as toolbox_markers).
constexpr const char* kMarkerMetadata = R"({"object_type":"plot_markers"})";

// --- PlotMarker -> chart-preview marker conversion --------------------------
// Mirrors PlotMarkersItem's severity palette so the preview matches the plot.
std::string colorHex(int r, int g, int b) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return std::string(buf);
}

std::string markerColorHex(const PJ::sdk::PlotMarker& m) {
  if (m.color.a != 0) {
    return colorHex(m.color.r, m.color.g, m.color.b);
  }
  switch (m.severity) {
    case PJ::sdk::MarkerSeverity::kInfo:
      return colorHex(80, 140, 255);
    case PJ::sdk::MarkerSeverity::kWarning:
      return colorHex(240, 180, 40);
    case PJ::sdk::MarkerSeverity::kError:
      return colorHex(230, 70, 60);
    case PJ::sdk::MarkerSeverity::kCritical:
      return colorHex(170, 30, 160);
  }
  return colorHex(80, 140, 255);
}

const char* markerKindName(PJ::sdk::MarkerKind k) {
  switch (k) {
    case PJ::sdk::MarkerKind::kRegion:
      return "region";
    case PJ::sdk::MarkerKind::kEvent:
      return "event";
    case PJ::sdk::MarkerKind::kValueBand:
      return "value_band";
    case PJ::sdk::MarkerKind::kLabel:
      return "label";
  }
  return "event";
}

// ---------------------------------------------------------------------------
// AnomalyDetectorDialog — Filter-Editor-style UI (preview / source / function / editor).
// ---------------------------------------------------------------------------

class AnomalyDetectorDialog : public PJ::DialogPluginTyped {
 public:
  std::string manifest() const override {
    return kAnomalyDetectorManifest;
  }
  std::string ui_content() const override {
    return kAnomalyDetectorDialogUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;
    wd.setListItems("source_list", series_names_);

    std::vector<std::string> function_names;
    for (const auto& f : anomaly_core::builtinFunctions()) {
      function_names.emplace_back(f.name);
    }
    wd.setListItems("function_list", function_names);

    if (!selected_source_.empty()) {
      wd.setSelectedItems("source_list", std::vector<std::string>{selected_source_});
    }
    wd.setCodeContent("code_editor", code_).setCodeLanguage("code_editor", "lua");
    wd.setChecked("global_marker", global_);
    // Save and Load are both native file pickers; onFileSelected tells them apart
    // by widget name. Save-as lets the user create a new file anywhere.
    wd.setSaveFilePicker("save_rule_button", "Save rule as...", "*.json", "Save detection rule");
    wd.setFilePicker("load_rule_button", "Load rule...", "*.json", "Load detection rule");
    wd.setText("status_label", status_);
    wd.setEnabled("apply_button", !code_.empty());

    // Preview: the selected source curve, with the rule's detected markers overlaid.
    if (!selected_source_.empty() && preview_provider_) {
      PJ::ChartSeries cs;
      cs.label = selected_source_;
      cs.points = preview_provider_(selected_source_);
      wd.setChartSeries("preview_chart", std::vector<PJ::ChartSeries>{cs});
      // Always push markers (empty when the rule yields none) so a source/rule change
      // clears the previous overlay instead of leaving stale markers behind.
      if (marker_provider_) {
        wd.setChartMarkers("preview_chart", marker_provider_(code_, selected_source_));
      }
    }
    return wd.toJson();
  }

  bool onSelectionChanged(std::string_view name, const std::vector<std::string>& items) override {
    if (name == "source_list") {
      selected_source_ = items.empty() ? "" : items.front();
      // Re-target the rule at the new source so the preview recomputes for it (and
      // stale markers from the previous series don't linger). Manual edits are kept.
      if (!current_template_.empty() && !user_edited_) {
        code_ = anomaly_core::substituteSource(current_template_, selected_source_);
      }
      return true;  // refresh preview + markers
    }
    if (name == "function_list") {
      const std::string fn = items.empty() ? "" : items.front();
      for (const auto& f : anomaly_core::builtinFunctions()) {
        if (fn == f.name) {
          current_template_ = f.code;
          user_edited_ = false;
          code_ = anomaly_core::substituteSource(f.code, selected_source_);
          break;
        }
      }
      return true;  // load the template into the editor
    }
    return false;
  }

  bool onCodeChanged(std::string_view name, std::string_view code) override {
    if (name == "code_editor") {
      code_ = std::string(code);  // user edits win over the template
      user_edited_ = true;        // ...and survive a later source change
    }
    return false;  // no widget_data re-read while typing
  }

  // Called by the panel host ~20Hz; lets the toolbox refresh its catalog so a
  // dataset loaded after the panel opened shows up in the source list live.
  bool onTick() override {
    if (on_tick_) {
      on_tick_();
    }
    return false;  // the panel byte-diffs widget_data(); no forced re-read needed
  }

  bool onToggled(std::string_view name, bool checked) override {
    if (name == "global_marker") {
      global_ = checked;
    }
    return false;
  }

  bool onClicked(std::string_view name) override {
    if (name == "apply_button" && !code_.empty() && run_callback_) {
      try {
        status_ = run_callback_(code_, selected_source_, global_);
      } catch (const std::exception& e) {
        status_ = std::string("Error: ") + e.what();
      } catch (...) {
        status_ = "Error: unknown exception";
      }
      return true;
    }
    return false;
  }

  // Save and Load are both native file pickers (setSaveFilePicker / setFilePicker);
  // the host returns the chosen path here, and we branch on the widget name.
  bool onFileSelected(std::string_view name, std::string_view path) override {
    if (name == "save_rule_button") {
      status_ = saveRuleTo(std::string(path));
      return true;
    }
    if (name == "load_rule_button") {
      std::ifstream in(std::string{path});
      if (!in) {
        status_ = "Error: cannot read " + std::string(path);
        return true;
      }
      std::stringstream ss;
      ss << in.rdbuf();
      std::string err;
      const auto rule = anomaly_core::ruleFromJson(ss.str(), &err);
      if (!rule) {
        status_ = "Error: " + err;
        return true;
      }
      code_ = rule->code;
      selected_source_ = rule->source;
      status_ = "Loaded rule from " + std::string(path);
      return true;
    }
    return false;
  }

  // Workspace persistence: the rule survives closing/reopening the dialog (and is
  // saved into the layout). The toolbox forwards saveConfig/loadConfig here.
  std::string saveConfig() const override {
    nlohmann::json j;
    j["code"] = code_;
    j["source"] = selected_source_;
    j["global"] = global_;
    return j.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    const nlohmann::json j = nlohmann::json::parse(config_json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      return false;
    }
    code_ = j.value("code", code_);
    selected_source_ = j.value("source", selected_source_);
    global_ = j.value("global", global_);
    return true;
  }

  void setSeriesNames(std::vector<std::string> names) {
    series_names_ = std::move(names);
    // Auto-select the first source so the preview fills on open (like the Filter
    // Editor, which is launched with a source curve already selected).
    if (selected_source_.empty() && !series_names_.empty()) {
      selected_source_ = series_names_.front();
      // Target the pristine template at the auto-selected source so the first preview
      // is correct (not left pointing at an empty --SOURCE-- substitution).
      if (!current_template_.empty() && !user_edited_) {
        code_ = anomaly_core::substituteSource(current_template_, selected_source_);
      }
    }
  }

  using RunCallback = std::function<std::string(const std::string& code, const std::string& source, bool global)>;
  void setRunCallback(RunCallback cb) {
    run_callback_ = std::move(cb);
  }

  using PreviewProvider = std::function<std::vector<PJ::ChartPoint>(const std::string& series_name)>;
  void setPreviewProvider(PreviewProvider cb) {
    preview_provider_ = std::move(cb);
  }

  // Runs the current rule and returns the detected markers, rebased to the preview's
  // X units, so the preview can overlay what Apply would publish.
  using MarkerProvider =
      std::function<std::vector<PJ::ChartMarker>(const std::string& code, const std::string& source)>;
  void setMarkerProvider(MarkerProvider cb) {
    marker_provider_ = std::move(cb);
  }

  void setOnTick(std::function<void()> cb) {
    on_tick_ = std::move(cb);
  }

 private:
  // Write the current rule (script + source) to the chosen path as a portable JSON
  // file. The Save-as picker already let the user create/select the file; we just
  // ensure a .json extension.
  std::string saveRuleTo(std::string path) const {
    if (path.empty()) {
      return "Error: no file selected";
    }
    if (path.size() < 5 || path.substr(path.size() - 5) != ".json") {
      path += ".json";
    }
    anomaly_core::Rule rule;
    rule.code = code_;
    rule.source = selected_source_;
    std::ofstream out(path);
    if (!out) {
      return "Error: cannot write " + path;
    }
    out << anomaly_core::ruleToJson(rule);
    return "Saved rule to " + path;
  }

  // "-- No function --" guidance is builtin function index 0.
  std::string code_ = anomaly_core::substituteSource(anomaly_core::builtinFunctions().front().code, "");
  std::string status_ = "Pick a source curve and a function (or write your own), then Apply.";
  std::string selected_source_;
  // Raw builtin code (still containing --SOURCE--) of the active function, so a source
  // change can re-target the rule; user_edited_ guards manual edits from being clobbered.
  std::string current_template_ = anomaly_core::builtinFunctions().front().code;
  bool user_edited_ = false;
  bool global_ = false;
  std::vector<std::string> series_names_;
  RunCallback run_callback_;
  PreviewProvider preview_provider_;
  MarkerProvider marker_provider_;
  std::function<void()> on_tick_;
};

// ---------------------------------------------------------------------------
// AnomalyDetectorToolbox — reads series, runs the rule (anomaly_core), publishes markers.
// ---------------------------------------------------------------------------

class AnomalyDetectorToolbox : public PJ::ToolboxPluginBase {
 public:
  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    dialog_.setRunCallback([this](const std::string& code, const std::string& source, bool global) {
      return runScript(code, source, global);
    });
    dialog_.setPreviewProvider([this](const std::string& name) { return previewPoints(name); });
    dialog_.setMarkerProvider(
        [this](const std::string& code, const std::string& source) { return previewMarkers(code, source); });
    // Refresh the catalog on every panel tick (cheap unless the catalog changed) so a
    // dataset loaded after the panel opened appears in the source list without reopening.
    dialog_.setOnTick([this]() { refreshCatalogIfChanged(); });
    refreshCatalogIfChanged();  // initial fill
    return PJ::borrowDialog(dialog_);
  }

  // Persist the dialog's rule into the workspace layout (and restore it on load).
  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }
  PJ::Status loadConfig(std::string_view config_json) override {
    dialog_.loadConfig(config_json);
    return PJ::okStatus();
  }

 private:
  // Cheap guard around the expensive refreshCatalog(): only re-read all series when
  // the catalog metadata actually changed (dataset/series added or removed). Safe to
  // call every panel tick. catalogSnapshot() is a zero-copy metadata view.
  void refreshCatalogIfChanged() {
    if (!toolboxHostBound()) {
      return;
    }
    auto catalog = toolboxHost().catalogSnapshot();
    if (!catalog) {
      return;
    }
    std::uint64_t sig = 1469598103934665603ull;  // FNV offset basis as a seed
    for (const auto& ds : catalog->dataSources()) {
      sig = (sig ^ ds.handle.id) * 1099511628211ull;
    }
    for (const auto& topic : catalog->topics()) {
      sig = (sig ^ (static_cast<std::uint64_t>(topic.source.id) << 32 | topic.field_count)) * 1099511628211ull;
    }
    if (sig == last_catalog_sig_) {
      return;
    }
    last_catalog_sig_ = sig;
    refreshCatalog();
  }

  void refreshCatalog() {
    if (!toolboxHostBound()) {
      return;
    }
    auto host = toolboxHost();
    auto catalog = host.catalogSnapshot();
    if (!catalog) {
      return;
    }
    series_map_.clear();
    series_names_.clear();
    series_dataset_.clear();
    dataset_ids_.clear();
    for (const auto& ds : catalog->dataSources()) {
      dataset_ids_.push_back(ds.handle.id);
    }
    const auto all_fields = catalog->fields();
    for (const auto& topic : catalog->topics()) {
      const std::string topic_name(PJ::sdk::toStringView(topic.name));
      for (uint32_t fi = topic.first_field; fi < topic.first_field + topic.field_count; ++fi) {
        const auto& f = all_fields[fi];
        // Build the key with markerSeriesKey so it matches the plot overlay's
        // per-series key exactly (so per-series markers land on the right curve).
        std::string name = PJ::sdk::markerSeriesKey(topic_name, PJ::sdk::toStringView(f.name));
        series_names_.push_back(name);
        series_dataset_[name] = topic.source.id;
        auto series = host.readSeries(f.handle);
        if (series && series->type() == PJ::PrimitiveType::kFloat64) {
          const auto ts = series->timestamps();
          const double* values = series->valuesAsFloat64();
          const size_t count = ts.size();
          if (values != nullptr) {
            anomaly_core::SeriesAccessor sa;
            sa.timestamps.resize(count);
            for (size_t i = 0; i < count; ++i) {
              sa.timestamps[i] = static_cast<double>(ts[i]);  // int64 ns -> double
            }
            sa.values.assign(values, values + count);  // already double: bulk copy
            series_map_[name] = std::move(sa);
          }
        }
      }
    }
    dialog_.setSeriesNames(series_names_);
  }

  // Preview points for a series: time rebased to seconds-from-start, downsampled
  // to a manageable count for the chart.
  std::vector<PJ::ChartPoint> previewPoints(const std::string& name) {
    std::vector<PJ::ChartPoint> pts;
    auto it = series_map_.find(name);
    if (it == series_map_.end() || it->second.timestamps.empty()) {
      return pts;
    }
    const auto& sa = it->second;
    const double t0 = sa.timestamps.front();
    const size_t n = sa.timestamps.size();
    constexpr size_t kMaxPoints = 2000;
    const size_t stride = (n > kMaxPoints) ? (n / kMaxPoints) : 1;
    pts.reserve(n / stride + 1);
    for (size_t i = 0; i < n; i += stride) {
      pts.push_back(PJ::ChartPoint{(sa.timestamps[i] - t0) / 1e9, sa.values[i]});
    }
    return pts;
  }

  // A SeriesProvider backed by the current series_map_. The get-lambda captures this,
  // so the result must be used synchronously (while series_map_ stays stable).
  anomaly_core::SeriesProvider makeSeriesProvider() const {
    anomaly_core::SeriesProvider provider;
    provider.names = series_names_;
    provider.get = [this](const std::string& name) -> const anomaly_core::SeriesAccessor* {
      auto it = series_map_.find(name);
      return it == series_map_.end() ? nullptr : &it->second;
    };
    return provider;
  }

  // Run the current rule and return its markers in the preview's X units (seconds-
  // from-start, the same t0 as previewPoints) so they overlay the curve. A bad rule
  // (or a source with no data) yields no overlay — the preview just shows the curve.
  std::vector<PJ::ChartMarker> previewMarkers(const std::string& code, const std::string& source) {
    std::vector<PJ::ChartMarker> out;
    auto it = series_map_.find(source);
    if (code.empty() || it == series_map_.end() || it->second.timestamps.empty()) {
      return out;
    }
    const double t0 = it->second.timestamps.front();

    const anomaly_core::SeriesProvider provider = makeSeriesProvider();
    std::string err;
    const std::vector<PJ::sdk::PlotMarker> markers = anomaly_core::runAnomalyScript(code, provider, &err);
    if (!err.empty()) {
      return out;
    }

    out.reserve(markers.size());
    for (const auto& m : markers) {
      PJ::ChartMarker cm;
      cm.kind = markerKindName(m.kind);
      cm.x0 = (static_cast<double>(m.t_start) - t0) / 1e9;
      cm.x1 = (static_cast<double>(m.t_end) - t0) / 1e9;
      cm.y0 = m.value_low;
      cm.y1 = m.value_high;
      cm.has_value = m.has_value;
      cm.color = markerColorHex(m);
      cm.label = m.label;
      out.push_back(std::move(cm));
    }
    return out;
  }

  // Refresh series, run the rule through the shared engine, publish the markers.
  std::string runScript(const std::string& code, const std::string& source, bool global) {
    if (code.empty()) {
      return "Error: empty rule";
    }
    if (!toolboxHostBound() || !runtimeHostBound()) {
      return "Error: toolbox/runtime host not bound";
    }
    refreshCatalog();

    const anomaly_core::SeriesProvider provider = makeSeriesProvider();
    std::string err;
    const std::vector<PJ::sdk::PlotMarker> markers = anomaly_core::runAnomalyScript(code, provider, &err);
    if (!err.empty()) {
      return "Error: " + err;
    }
    return publishMarkers(markers, source, global);
  }

  // Publish the emitted set. Global: a dataset-global topic on every loaded dataset
  // (shows on all plots). Per-series: under the selected source curve's own key on its
  // dataset (shows only on plots of that curve). Keep-latest retention either way.
  std::string publishMarkers(const std::vector<PJ::sdk::PlotMarker>& markers, const std::string& source, bool global) {
    if (dataset_ids_.empty()) {
      return "Error: no dataset loaded";
    }
    auto host = toolboxHost();
    PJ::sdk::PlotMarkers set;
    set.markers = markers;
    const std::vector<uint8_t> bytes = PJ::serializePlotMarkers(set);

    // Resolve (dataset, topic-key) targets.
    std::vector<std::pair<PJ::DatasetId, std::string>> targets;
    if (global) {
      const std::string key(PJ::sdk::kGlobalMarkerTopic);
      for (const PJ::DatasetId did : dataset_ids_) {
        targets.emplace_back(did, key);
      }
    } else {
      if (source.empty()) {
        return "Error: select a source curve, or tick Global marker";
      }
      PJ::DatasetId did = dataset_ids_.front();
      if (const auto it = series_dataset_.find(source); it != series_dataset_.end()) {
        did = it->second;
      }
      targets.emplace_back(did, source);
    }

    for (const auto& [did, key] : targets) {
      const std::string topic = PJ::sdk::markerObjectTopicName(key);
      auto handle = host.registerObjectTopicOnDataset(did, topic, kMarkerMetadata);
      if (!handle) {
        return "Error: register: " + handle.error();
      }
      (void)host.setObjectTopicRetention(*handle, 1);
      auto status =
          host.pushOwnedObject(*handle, PJ::Timestamp{0}, PJ::Span<const uint8_t>{bytes.data(), bytes.size()});
      if (!status) {
        return "Error: push: " + status.error();
      }
    }
    if (runtimeHostBound()) {
      runtimeHost().notifyDataChanged();
    }
    return "Done: " + std::to_string(markers.size()) + " marker(s)" + (global ? " (global)" : " on " + source);
  }

  AnomalyDetectorDialog dialog_;
  std::unordered_map<std::string, anomaly_core::SeriesAccessor> series_map_;
  std::vector<std::string> series_names_;
  std::map<std::string, PJ::DatasetId> series_dataset_;  // markerSeriesKey -> owning dataset
  std::vector<PJ::DatasetId> dataset_ids_;
  std::uint64_t last_catalog_sig_ = UINT64_MAX;  // forces the first refresh; guards live ticks
};

}  // namespace

PJ_TOOLBOX_PLUGIN(AnomalyDetectorToolbox, kAnomalyDetectorManifest)
PJ_DIALOG_PLUGIN(AnomalyDetectorDialog)
