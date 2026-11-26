// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// Marker Editor toolbox: a preview chart over a Source | Function | Lua editor. Apply submits the rule to
// the host (`pj.data_processors.v1`, kind=markers), which runs it and publishes the resulting PlotMarkers
// (per series, or dataset-wide / globally by scope). The plugin runs no Lua itself.
//
// A Toolbox because only that plugin family can read timeseries (catalogSnapshot + readSeries) and write
// objects.

#include <algorithm>
#include <cctype>
#include <cstddef>
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
#include <pj_utf8_path/utf8_path.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "anomaly_detector_dialog_ui.hpp"
#include "anomaly_detector_manifest.hpp"
#include "core/anomaly_helpers.hpp"
#include "toolbox_preview/ephemeral_preview.hpp"  // PreviewBackend, EphemeralPreview, PreviewOverlay
#include "toolbox_preview/series_catalog.hpp"     // SeriesCatalog

namespace {

// PlotMarkers -> chart overlay. toolbox_preview renders only the input series, so the output mapping lives here.

// Cap on markers drawn in the LIVE preview: a noisy rule can emit one marker per sample and the panel
// re-serializes the overlay to JSON every 20 Hz tick. Committed (Apply) markers are not capped.
constexpr std::size_t kPreviewMarkerCap = 2000;

std::string colorHex(int r, int g, int b) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return std::string(buf);
}

// Mirrors PlotMarkersItem's severity palette so the preview matches the plot overlay.
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

// Convert a host-published PlotMarkers set into chart-overlay markers, rebased to seconds from `t0_ns`.
// Over `max_markers`, stride-decimate (like the source curve's downsampleToChart).
std::vector<PJ::ChartMarker> markersToChart(
    const PJ::sdk::PlotMarkers& markers, double t0_ns, std::size_t max_markers = kPreviewMarkerCap) {
  const std::size_t n = markers.markers.size();
  const std::size_t stride = (max_markers > 0 && n > max_markers) ? (n / max_markers) : 1;
  std::vector<PJ::ChartMarker> out;
  out.reserve(n / stride + 1);
  for (std::size_t i = 0; i < n; i += stride) {
    const auto& m = markers.markers[i];
    PJ::ChartMarker cm;
    cm.kind = markerKindName(m.kind);
    cm.x0 = (static_cast<double>(m.t_start) - t0_ns) / 1e9;
    cm.x1 = (static_cast<double>(m.t_end) - t0_ns) / 1e9;
    cm.y0 = m.value_low;
    cm.y1 = m.value_high;
    cm.has_value = m.has_value;
    cm.color = markerColorHex(m);
    cm.label = m.label;
    out.push_back(std::move(cm));
  }
  return out;
}

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
    // Accept series dragged from the host's Datasets tree. Set on the section frame so the whole section is
    // a target. PanelEngine reads drop targets once, from the initial payload, so this must be in the first
    // delivery.
    wd.setDropTarget("sourceGroup");
    const std::vector<std::string>& visible = visibleNames();
    wd.setListItems("source_list", visible);
    // Say WHY the list is empty; "no series match" and "nothing loaded" send the user to
    // completely different places.
    if (series_names_.empty()) {
      wd.setListPlaceholder("source_list", "No series loaded");
    } else if (visible.empty()) {
      wd.setListPlaceholder("source_list", "No series match \"" + filter_ + "\"");
    } else {
      wd.setListPlaceholder("source_list", "Drag & drop a series here, or pick one from the list");
    }

    std::vector<std::string> function_names;
    for (const auto& f : anomaly_core::builtinFunctions()) {
      function_names.emplace_back(f.name);
    }
    wd.setListItems("function_list", function_names);

    if (!selected_source_.empty()) {
      wd.setSelectedItems("source_list", std::vector<std::string>{selected_source_});
    }
    wd.setCodeContent("code_editor", code_).setCodeLanguage("code_editor", "lua");
    // Scope is one ordered axis (series, dataset, every dataset) carried by the two persisted booleans;
    // naming the segments makes the invalid pair (all_datasets without global) unreachable.
    wd.setChecked("scope_timeseries", !global_);
    wd.setChecked("scope_dataset", global_ && !all_datasets_);
    wd.setChecked("scope_global", global_ && all_datasets_);
    wd.setSaveFilePicker("save_rule_button", "Save rule as...", "*.json", "Save detection rule", "json");
    wd.setFilePicker("load_rule_button", "Load rule...", "*.json", "Load detection rule");
    // State the precondition in the UI rather than failing on the click: runScript needs a source unless
    // the scope is Dataset or Global.
    const bool resolved = sourceResolved();
    wd.setEnabled("apply_button", !code_.empty() && (global_ || resolved));

    std::string status_line = status_;
    if (!selected_source_.empty() && !resolved) {
      status_line = "Source '" + selected_source_ + "' is not available in the loaded datasets.";
    }
    if (resolved && preview_provider_) {
      PJ::ChartSeries cs;
      cs.label = selected_source_;
      cs.points = preview_provider_(selected_source_);
      wd.setChartSeries("preview_chart", std::vector<PJ::ChartSeries>{cs});
      // Interactive zoom (rubber band + mouse wheel) on the host ChartPreviewWidget; off by
      // default, so it must be re-declared each tick while the chart is shown.
      wd.setChartZoomEnabled("preview_chart", true);
      // Always push markers (empty when the rule yields none) so a source/rule change
      // clears the previous overlay instead of leaving stale markers behind.
      if (marker_provider_) {
        std::string preview_error;
        wd.setChartMarkers("preview_chart", marker_provider_(code_, selected_source_, preview_error));
        // Surface a rule compile/runtime error so a blank overlay isn't mistaken for
        // "no anomalies found".
        if (!preview_error.empty()) {
          status_line = "Rule error: " + preview_error;
        }
      }
    } else {
      // No usable source: clear the chart, or the previous curve stays painted under a status line that
      // says there is no source.
      wd.clearChart("preview_chart");
      wd.setChartPlaceholder("preview_chart", "Pick a source series, or drag & drop one from the Datasets tree");
    }
    wd.setText("status_label", status_line);
    return wd.toJson();
  }

  bool onSelectionChanged(std::string_view name, const std::vector<std::string>& items) override {
    if (name == "source_list") {
      // Re-target the rule at the new source so the preview recomputes for it (and
      // stale markers from the previous series don't linger). Manual edits are kept.
      return adoptSource(items.empty() ? std::string{} : items.front());  // refresh preview + markers
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

  // Series dragged from the host's Datasets tree onto the Source section. Validated against series_names_
  // (everything the catalog offers), not the filtered view: a drop has to work while a text filter is
  // narrowing the list.
  //
  // A text field arrives as an unresolved catalog key (the host's catalog_key_resolver finds no plottable
  // curve for it), so it fails the lookup and is indistinguishable from an unknown drop; one message
  // covers both.
  bool onItemsDropped(std::string_view name, const std::vector<std::string>& items) override {
    if (name != "sourceGroup" || items.empty()) {
      return false;
    }
    const std::string& dropped = items.front();
    if (std::find(series_names_.begin(), series_names_.end(), dropped) == series_names_.end()) {
      status_ =
          "'" + dropped + "' is unavailable as a source: a detection rule needs a numeric or boolean time series.";
      return true;  // repaint the status; leave the selection alone
    }
    adoptSource(dropped);
    status_ =
        items.size() > 1 ? "Selected '" + dropped + "' (one source series at a time)" : "Selected '" + dropped + "'";
    return true;
  }

  // The filter lives inside the Source section's band (sourceHeader's filterFieldName). Purely a view
  // filter: it must never touch the selection or the rule, so narrowing the list cannot change what Apply
  // would run.
  bool onTextChanged(std::string_view name, std::string_view text) override {
    if (name == "source_filter") {
      filter_ = std::string(text);
      list_dirty_ = true;
      return true;
    }
    return false;
  }

  bool onCodeChanged(std::string_view name, std::string_view code) override {
    // A code event that changes nothing is not an edit. The host emits one on the panel's first paint (the
    // syntax highlighter's deferred rehighlight), and taking it as a user edit would pin the pristine
    // template to "--SOURCE--" until a function is picked.
    if (name == "code_editor" && code != code_) {
      code_ = std::string(code);  // user edits win over the template
      user_edited_ = true;        // ...and survive a later source change
    }
    return false;  // no widget_data re-read while typing
  }

  // Called by the panel host at ~20 Hz; refreshes the catalog so a dataset loaded after the panel opened
  // shows up in the source list live.
  bool onTick() override {
    if (on_tick_) {
      on_tick_();
    }
    return false;  // the panel byte-diffs widget_data(); no forced re-read needed
  }

  // A radio group reports the segment that turned OFF as well as the one that turned ON;
  // only the latter carries the new scope.
  bool onToggled(std::string_view name, bool checked) override {
    if (!checked) {
      return false;
    }
    if (name == "scope_timeseries") {
      global_ = false;
      all_datasets_ = false;
    } else if (name == "scope_dataset") {
      global_ = true;
      all_datasets_ = false;
    } else if (name == "scope_global") {
      global_ = true;
      all_datasets_ = true;
    } else {
      return false;
    }
    return true;  // Apply gating reads global_
  }

  bool onClicked(std::string_view name) override {
    if (name == "apply_button" && !code_.empty() && run_callback_) {
      try {
        status_ = run_callback_(code_, selected_source_, global_, all_datasets_);
      } catch (const std::exception& e) {
        status_ = std::string("Error: ") + e.what();
      } catch (...) {
        status_ = "Error: unknown exception";
      }
      return true;
    }
    return false;
  }

  // Save and Load are native file pickers; the host returns the chosen path here and we branch on the
  // widget name.
  bool onFileSelected(std::string_view name, std::string_view path) override {
    if (name == "save_rule_button") {
      status_ = saveRuleTo(std::string(path));
      return true;
    }
    if (name == "load_rule_button") {
      std::ifstream in(PJ::utf8_path::pathFromUtf8(path));
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
      // A loaded rule counts as hand-authored: otherwise picking a source afterwards would overwrite it
      // with the active builtin template.
      user_edited_ = true;
      status_ = "Loaded rule from " + std::string(path);
      return true;
    }
    return false;
  }

  // Persisted in the workspace layout (the toolbox forwards saveConfig/loadConfig here).
  std::string saveConfig() const override {
    nlohmann::json j;
    j["code"] = code_;
    j["source"] = selected_source_;
    j["global"] = global_;
    j["all_datasets"] = all_datasets_;
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
    all_datasets_ = j.value("all_datasets", all_datasets_);
    return true;
  }

  // Deliberately does not auto-select a source: the first catalog name is often an unusable field.
  //
  // A `selected_source_` restored from a layout is left alone even when absent from `names`: the dataset
  // may not have loaded yet, and clearing it would retarget the rule back to --SOURCE--. widget_data()
  // reports the unresolved state and it heals once the catalog catches up.
  void setSeriesNames(std::vector<std::string> names) {
    series_names_ = std::move(names);
    list_dirty_ = true;
  }

  using RunCallback =
      std::function<std::string(const std::string& code, const std::string& source, bool global, bool all_datasets)>;
  void setRunCallback(RunCallback cb) {
    run_callback_ = std::move(cb);
  }

  using PreviewProvider = std::function<std::vector<PJ::ChartPoint>(const std::string& series_name)>;
  void setPreviewProvider(PreviewProvider cb) {
    preview_provider_ = std::move(cb);
  }

  // Runs the current rule and returns the detected markers rebased to the preview's X units. `error` is
  // set to a rule compile/runtime error (empty on success).
  using MarkerProvider = std::function<std::vector<PJ::ChartMarker>(
      const std::string& code, const std::string& source, std::string& error)>;
  void setMarkerProvider(MarkerProvider cb) {
    marker_provider_ = std::move(cb);
  }

  void setOnTick(std::function<void()> cb) {
    on_tick_ = std::move(cb);
  }

  // The selected source series; the toolbox's tick handler live-refreshes just that series while streaming.
  const std::string& selectedSource() const {
    return selected_source_;
  }

 private:
  // Whether the host can actually run a rule against the selection: false for "nothing picked" and for a
  // layout-restored name the catalog no longer offers.
  [[nodiscard]] bool sourceResolved() const {
    return !selected_source_.empty() &&
           std::find(series_names_.begin(), series_names_.end(), selected_source_) != series_names_.end();
  }

  // Take `name` as the source, retargeting a builtin template at it (shared by list click and drop).
  bool adoptSource(std::string name) {
    selected_source_ = std::move(name);
    // Manual edits win: only a pristine template gets re-pointed.
    if (!current_template_.empty() && !user_edited_) {
      code_ = anomaly_core::substituteSource(current_template_, selected_source_);
    }
    return true;
  }

  static void foldAsciiInto(std::string_view text, std::string& out) {
    out.clear();  // keeps the capacity, so a per-name fold does not allocate every time
    for (const char c : text) {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }

  // Case-insensitive substring match on the whitespace-trimmed query, in catalog order. Recomputed only
  // when the list or the query changed: widget_data() runs at ~20 Hz and the list can be thousands of names.
  const std::vector<std::string>& visibleNames() {
    if (!list_dirty_) {
      return visible_names_;
    }
    list_dirty_ = false;
    visible_names_.clear();

    const std::size_t begin = filter_.find_first_not_of(" \t\n\r\f\v");
    if (begin == std::string::npos) {
      visible_names_ = series_names_;
      return visible_names_;
    }
    const std::size_t end = filter_.find_last_not_of(" \t\n\r\f\v");
    std::string query;
    foldAsciiInto(std::string_view(filter_).substr(begin, end - begin + 1), query);

    std::string folded;
    for (const std::string& name : series_names_) {
      foldAsciiInto(name, folded);
      if (folded.find(query) != std::string::npos) {
        visible_names_.push_back(name);
      }
    }
    return visible_names_;
  }

  // Write the current rule (script + source) to the chosen path as portable JSON. The host's
  // save-file-picker default_suffix contract guarantees the ".json" extension.
  std::string saveRuleTo(std::string path) const {
    if (path.empty()) {
      return "Error: no file selected";
    }
    anomaly_core::Rule rule;
    rule.code = code_;
    rule.source = selected_source_;
    std::ofstream out(PJ::utf8_path::pathFromUtf8(path));
    if (!out) {
      return "Error: cannot write " + path;
    }
    out << anomaly_core::ruleToJson(rule);
    return "Saved rule to " + path;
  }

  // "-- No function --" guidance is builtin function index 0.
  std::string code_ = anomaly_core::substituteSource(anomaly_core::builtinFunctions().front().code, "");
  std::string status_ = "Pick a source series (or drag & drop one), pick a function, then Apply.";
  std::string selected_source_;
  // Source-list view state. Transient by design: a stale filter restored with a layout
  // would look like a broken list, so none of this is persisted.
  std::string filter_;
  std::vector<std::string> visible_names_;
  bool list_dirty_ = true;
  // Raw builtin code (still containing --SOURCE--) of the active function, so a source
  // change can re-target the rule; user_edited_ guards manual edits from being clobbered.
  std::string current_template_ = anomaly_core::builtinFunctions().front().code;
  bool user_edited_ = false;
  bool global_ = false;
  bool all_datasets_ = false;  // with global_: the Global scope, spanning every loaded dataset
  std::vector<std::string> series_names_;
  RunCallback run_callback_;
  PreviewProvider preview_provider_;
  MarkerProvider marker_provider_;
  std::function<void()> on_tick_;
};

// Stable plugin-local id for the single live preview generator (created EPHEMERAL,
// torn down with remove()). The host namespaces it under this plugin.
constexpr std::string_view kPreviewId = "preview";

// The toolbox is the preview's `PreviewBackend`: the shared `EphemeralPreview` calls back into
// submitPreview/readPreviewOverlay/removePreview, the marker-specific part (which host service, how it renders).
class AnomalyDetectorToolbox : public PJ::ToolboxPluginBase, public toolbox_preview::PreviewBackend {
 public:
  // Tears down the live preview's ephemeral object topic on unload. The host and its markers service
  // outlive the plugin instance during teardown; an unbound service yields no view and the call is skipped.
  ~AnomalyDetectorToolbox() override {
    removePreview();  // tear down the ephemeral preview generator
  }

  uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    dialog_.setRunCallback([this](const std::string& code, const std::string& source, bool global, bool all_datasets) {
      return runScript(code, source, global, all_datasets);
    });
    dialog_.setPreviewProvider([this](const std::string& name) { return catalog_.points(name); });
    dialog_.setMarkerProvider([this](const std::string& code, const std::string& source, std::string& error) {
      std::vector<PJ::ChartMarker> markers = previewMarkers(code, source);
      error = preview_error_;
      return markers;
    });
    // Refresh the catalog on every panel tick (cheap unless the structure changed) so a dataset loaded
    // after the panel opened appears without reopening, then live-refresh the selected source's samples.
    dialog_.setOnTick([this]() { onTick(); });
    onTick();  // initial fill
    return PJ::borrowDialog(dialog_);
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }
  PJ::Status loadConfig(std::string_view config_json) override {
    dialog_.loadConfig(config_json);
    return PJ::okStatus();
  }

 private:
  // Panel tick (~20 Hz): refresh the source list when the catalog STRUCTURE changes and the selected
  // source's samples so the streaming preview follows. Both are cheap no-ops when nothing changed.
  void onTick() {
    if (!toolboxHostBound()) {
      return;
    }
    if (catalog_.refreshStructureIfChanged(toolboxHost())) {
      dialog_.setSeriesNames(catalog_.names());
      preview_.markDirty();  // a new series the rule references may have appeared
    }
    catalog_.refreshSelectedSourceData(toolboxHost(), dialog_.selectedSource());
  }

  // The dialog's marker overlay: run the rule as an ephemeral generator (change-gated by
  // EphemeralPreview) and read the host-published markers back, rebased to the source's
  // t0. `preview_error_` carries a rule compile/runtime error for the status line.
  std::vector<PJ::ChartMarker> previewMarkers(const std::string& code, const std::string& source) {
    if (code.empty() || !catalog_.has(source)) {
      preview_error_.clear();
      return {};
    }
    toolbox_preview::PreviewOverlay overlay =
        preview_.refresh(*this, code, source, catalog_.t0(source), catalog_.dataRevision(), &preview_error_);
    return std::move(overlay.markers);
  }

  // Submit/replace the ephemeral marker generator (kind=markers, EPHEMERAL): the host
  // runs the rule, auto-names a preview marker object topic, and returns it. An empty
  // string means "not available right now" (host service unbound) — retried next tick.
  PJ::Expected<std::string> submitPreview(const std::string& code) override {
    auto gens = services().get<PJ::sdk::DataProcessorsHostService>();
    if (!gens) {
      return std::string{};
    }
    // Declare ONLY the series the rule reads (anomaly_core::ruleInputs): the host materializes every
    // declared input whole-series on each run, so declaring the whole catalog would re-materialize the
    // dataset on the GUI thread (the freeze). Built only on a re-submit, not every tick.
    const std::vector<std::string> refs = anomaly_core::ruleInputs(code, dialog_.selectedSource());
    const std::vector<std::string_view> inputs(refs.begin(), refs.end());
    const PJ::Expected<std::vector<std::string>> topics = gens->createMarkers(
        kPreviewId, inputs, /*output_marker_topic=*/"", code, "{}", PJ_DATA_PROCESSOR_FLAG_EPHEMERAL);
    if (!topics) {
      return PJ::unexpected(topics.error());  // compile/runtime rule error → surfaced
    }
    if (topics->empty()) {
      return std::string{};
    }
    return topics->front();
  }

  // Read the latest PlotMarkers from the preview object topic and render them as chart
  // markers. The host keeps this topic fresh on every commit, so between rule edits the
  // overlay just follows the stream (no script re-run here).
  toolbox_preview::PreviewOverlay readPreviewOverlay(const std::string& topic, double t0_ns) override {
    toolbox_preview::PreviewOverlay overlay;
    const PJ::sdk::ToolboxObjectReadHostView* reader = objectReadHost();
    if (reader == nullptr) {
      return overlay;
    }
    const std::optional<PJ::sdk::ObjectTopicHandle> handle = reader->lookupTopic(topic);
    if (!handle) {
      return overlay;
    }
    const PJ::Expected<PJ::sdk::ObjectBytes> bytes = reader->readLatestAt(*handle, PJ::Timestamp{0});
    if (!bytes || bytes->empty()) {
      return overlay;
    }
    const PJ::Span<const uint8_t> view = bytes->view();
    const PJ::Expected<PJ::sdk::PlotMarkers> decoded = PJ::deserializePlotMarkers(view.data(), view.size());
    if (!decoded) {
      return overlay;
    }
    overlay.markers = markersToChart(*decoded, t0_ns);
    return overlay;
  }

  void removePreview() override {
    if (auto gens = services().get<PJ::sdk::DataProcessorsHostService>()) {
      (void)gens->remove(kPreviewId);
    }
  }

  // Submit the rule to the host as a marker data processor (pj.data_processors.v1, kind=markers). The host
  // runs the script, publishes the PlotMarkers and re-runs them when data changes, so they survive this
  // plugin's unload.
  //
  // `all_datasets` (only with `global`) publishes the global markers across EVERY loaded dataset, else on
  // the active dataset only. The target stays the "__global__" topic either way; {"scope":"all"} is a hint
  // that a host which understands it remaps to its all-datasets marker topic, while an older host keeps
  // writing "__global__" everywhere, so the plugin works against either.
  std::string runScript(const std::string& code, const std::string& source, bool global, bool all_datasets) {
    if (code.empty()) {
      return "Error: empty rule";
    }
    if (!toolboxHostBound()) {
      return "Error: toolbox host not bound";
    }
    catalog_.refresh(toolboxHost());  // populate the series list so the host can resolve the inputs

    auto gens_service = services().get<PJ::sdk::DataProcessorsHostService>();
    if (!gens_service) {
      return "Error: host data-processors service (pj.data_processors.v1) is unavailable";
    }
    const PJ::sdk::DataProcessorsHostView gens = *gens_service;

    if (!global && source.empty()) {
      return "Error: select a source series, or set the scope to Dataset or Global";
    }
    const std::string target = global ? std::string(PJ::sdk::kGlobalMarkerTopic) : source;

    // Declare ONLY the series the rule reads (see anomaly_core::ruleInputs), same as the preview, so Apply
    // and preview stay identical and the host doesn't materialize the whole dataset on every run.
    const std::vector<std::string> refs = anomaly_core::ruleInputs(code, global ? std::string() : source);
    std::vector<std::string_view> inputs(refs.begin(), refs.end());

    // {"scope":"all"} tells the host to publish a global marker across every dataset.
    const bool global_all = global && all_datasets;
    const std::string params = global_all ? R"({"scope":"all"})" : "{}";

    // Stable per-(target, scope) id: re-Save upserts the generator for that rule. One id is one generator
    // on the host and a scope change on an id replaces it, so a Dataset rule and a Global rule can only
    // coexist under two ids. The host keeps their marker topics apart itself; the id split keeps the
    // RULES apart.
    const std::string id = "rule/" + (global_all ? std::string("__all__") : target);
    const PJ::Expected<std::vector<std::string>> submitted =
        gens.createMarkers(id, PJ::Span<const std::string_view>(inputs), target, code, params);
    if (!submitted) {
      return "Error: " + submitted.error();
    }
    if (runtimeHostBound()) {
      runtimeHost().notifyDataChanged();  // repaint overlays with the host-published set
    }
    const std::string scope = global ? (global_all ? " (global, all datasets)" : " (global)") : " on " + source;
    return "Done: host generator '" + id + "' submitted" + scope;
  }

  AnomalyDetectorDialog dialog_;
  // Shared "preview a plot" engine: source list + cached samples + streaming live-refresh, and the
  // change-gated ephemeral generator submit/read-back. Only the marker-specific render stays here.
  toolbox_preview::SeriesCatalog catalog_;
  toolbox_preview::EphemeralPreview preview_;
  std::string preview_error_;  // last preview rule error (empty on success), shown in the status
};

}  // namespace

PJ_TOOLBOX_PLUGIN(AnomalyDetectorToolbox, kAnomalyDetectorManifest)
PJ_DIALOG_PLUGIN(AnomalyDetectorDialog)
