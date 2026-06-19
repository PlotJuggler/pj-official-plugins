// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// Anomaly Detector toolbox (demo), styled after the host Filter Editor: a Preview
// chart on top, then Source curve | Function | Lua editor. Picking a predefined
// Function fills the editor with its Lua (targeting the selected source); the
// "-- No function --" entry leaves a blank template to write your own. Apply runs
// the Lua via the shared `anomaly_core` engine (sol2 + marker primitives) — the
// SAME engine the headless anomaly_runner CLI uses — and publishes the emitted set
// as a PlotMarkers object via the toolbox object-write surface (per-series under the
// selected source, or dataset-global when "Global marker" is ticked). The status
// shows "Done: N marker(s)" or "Error".
//
// It is a Toolbox because only that plugin family can read timeseries
// (catalogSnapshot + readSeries) and write objects. The detection logic itself is
// GUI-free and lives in anomaly_core; this file is only the host + dialog wiring.

#include <cstdint>
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
    wd.setText("rule_path", rule_path_);
    // Load is a file-picker button; Save writes to the rule_path field.
    wd.setFilePicker("load_rule_button", "Load rule...", "*.json", "Load detection rule");
    wd.setText("status_label", status_);
    wd.setEnabled("apply_button", !code_.empty());

    // Preview: the selected source curve.
    if (!selected_source_.empty() && preview_provider_) {
      PJ::ChartSeries cs;
      cs.label = selected_source_;
      cs.points = preview_provider_(selected_source_);
      wd.setChartSeries("preview_chart", std::vector<PJ::ChartSeries>{cs});
    }
    return wd.toJson();
  }

  bool onSelectionChanged(std::string_view name, const std::vector<std::string>& items) override {
    if (name == "source_list") {
      selected_source_ = items.empty() ? "" : items.front();
      return true;  // refresh preview
    }
    if (name == "function_list") {
      const std::string fn = items.empty() ? "" : items.front();
      for (const auto& f : anomaly_core::builtinFunctions()) {
        if (fn == f.name) {
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
    }
    return false;  // no widget_data re-read while typing
  }

  bool onToggled(std::string_view name, bool checked) override {
    if (name == "global_marker") {
      global_ = checked;
    }
    return false;
  }

  bool onTextChanged(std::string_view name, std::string_view text) override {
    if (name == "rule_path") {
      rule_path_ = std::string(text);
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
    if (name == "save_rule_button") {
      status_ = saveRuleToPath();
      return true;
    }
    return false;
  }

  // Load is wired as a file picker (setFilePicker); the host returns the chosen path here.
  bool onFileSelected(std::string_view name, std::string_view path) override {
    if (name == "load_rule_button") {
      rule_path_ = std::string(path);
      std::ifstream in(rule_path_);
      if (!in) {
        status_ = "Error: cannot read " + rule_path_;
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
      status_ = "Loaded rule from " + rule_path_;
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
    j["rule_path"] = rule_path_;
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
    rule_path_ = j.value("rule_path", rule_path_);
    return true;
  }

  void setSeriesNames(std::vector<std::string> names) {
    series_names_ = std::move(names);
    // Auto-select the first source so the preview fills on open (like the Filter
    // Editor, which is launched with a source curve already selected).
    if (selected_source_.empty() && !series_names_.empty()) {
      selected_source_ = series_names_.front();
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

 private:
  // Write the current rule (script + source) to rule_path_ as a portable JSON file.
  std::string saveRuleToPath() const {
    if (rule_path_.empty()) {
      return "Error: enter a rule file path first";
    }
    anomaly_core::Rule rule;
    rule.code = code_;
    rule.source = selected_source_;
    std::ofstream out(rule_path_);
    if (!out) {
      return "Error: cannot write " + rule_path_;
    }
    out << anomaly_core::ruleToJson(rule);
    return "Saved rule to " + rule_path_;
  }

  // "-- No function --" guidance is builtin function index 0.
  std::string code_ = anomaly_core::substituteSource(anomaly_core::builtinFunctions().front().code, "");
  std::string status_ = "Pick a source curve and a function (or write your own), then Apply.";
  std::string selected_source_;
  std::string rule_path_;
  bool global_ = false;
  std::vector<std::string> series_names_;
  RunCallback run_callback_;
  PreviewProvider preview_provider_;
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
    refreshCatalog();
    dialog_.setRunCallback([this](const std::string& code, const std::string& source, bool global) {
      return runScript(code, source, global);
    });
    dialog_.setPreviewProvider([this](const std::string& name) { return previewPoints(name); });
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

  // Refresh series, run the rule through the shared engine, publish the markers.
  std::string runScript(const std::string& code, const std::string& source, bool global) {
    if (code.empty()) {
      return "Error: empty rule";
    }
    if (!toolboxHostBound() || !runtimeHostBound()) {
      return "Error: toolbox/runtime host not bound";
    }
    refreshCatalog();

    anomaly_core::SeriesProvider provider;
    provider.names = series_names_;
    provider.get = [this](const std::string& name) -> const anomaly_core::SeriesAccessor* {
      auto it = series_map_.find(name);
      return it == series_map_.end() ? nullptr : &it->second;
    };

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
};

}  // namespace

PJ_TOOLBOX_PLUGIN(AnomalyDetectorToolbox, kAnomalyDetectorManifest)
PJ_DIALOG_PLUGIN(AnomalyDetectorDialog)
