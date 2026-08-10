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

// --- PlotMarkers -> chart overlay (this plugin's "render the generator's output") ---
// The generic toolbox_preview lib renders the INPUT source curve; rendering the OUTPUT is
// the backend's job, so the markers-specific mapping lives here, not in the shared lib.

// Upper bound on markers drawn in the LIVE preview. A noisy rule can emit one marker per
// sample; the panel re-serializes the overlay to JSON every 20 Hz tick, so an unbounded set
// pegs the UI. The committed (Apply) markers are NOT capped — only this in-dialog preview.
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

// Convert a host-published PlotMarkers set into chart-overlay markers, rebased to
// seconds-from-`t0_ns`. Over `max_markers`, stride-decimate (same approach as the source
// curve's downsampleToChart) to keep the per-tick serialization bounded.
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

// The series a rule reads, parsed from its literal series("...") / series('...') calls.
// The host materializes EVERY declared generator input whole-series on each run, so a
// generator must declare only what the rule actually touches — declaring the whole catalog
// re-materializes the entire dataset on the GUI thread (the preview-freeze cause). Dynamic
// names (series("x"..var)) aren't seen; anomaly rules use literal keys.
std::vector<std::string> parseSeriesRefs(const std::string& code) {
  std::vector<std::string> refs;
  const std::string token = "series(";
  std::size_t pos = 0;
  while ((pos = code.find(token, pos)) != std::string::npos) {
    pos += token.size();
    while (pos < code.size() && (code[pos] == ' ' || code[pos] == '\t')) {
      ++pos;
    }
    if (pos >= code.size() || (code[pos] != '"' && code[pos] != '\'')) {
      continue;  // not a string-literal argument
    }
    const char quote = code[pos++];
    const std::size_t start = pos;
    while (pos < code.size() && code[pos] != quote) {
      ++pos;
    }
    if (pos > start && pos < code.size()) {
      std::string name = code.substr(start, pos - start);
      if (std::find(refs.begin(), refs.end(), name) == refs.end()) {
        refs.push_back(std::move(name));  // dedup so the host materializes each series once
      }
    }
  }
  return refs;
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
    // Accept series dragged from the host's Datasets tree. Marked on the group box, not
    // the list, so the whole panel section is a target. This MUST be present in the very
    // first delivery: PanelEngine reads the drop targets once, off the initial payload —
    // it is not a per-tick flag.
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
    wd.setChecked("global_marker", global_);
    // "All datasets" only applies to a global marker — enabled when Global is ticked.
    wd.setChecked("all_datasets_marker", all_datasets_);
    wd.setEnabled("all_datasets_marker", global_);
    // Save and Load are both native file pickers; onFileSelected tells them apart
    // by widget name. Save-as lets the user create a new file anywhere.
    wd.setSaveFilePicker("save_rule_button", "Save rule as...", "*.json", "Save detection rule", "json");
    wd.setFilePicker("load_rule_button", "Load rule...", "*.json", "Load detection rule");
    // State the precondition in the UI rather than failing on the click: runScript needs a
    // source unless the markers are global. Reachable by default now that nothing is
    // auto-selected.
    const bool resolved = sourceResolved();
    wd.setEnabled("apply_button", !code_.empty() && (global_ || resolved));

    // Preview: the selected source curve, with the rule's detected markers overlaid.
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
      // No usable source: clear the chart, or the previous curve stays painted under a
      // status line that says there is no source. Now reachable on open, since nothing is
      // auto-selected any more.
      wd.clearChart("preview_chart");
      wd.setChartPlaceholder("preview_chart", "Pick a source curve, or drag & drop one from the Datasets tree");
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

  // Series dragged from the host's Datasets tree onto the Source curve box.
  //
  // Validated against series_names_ (everything the catalog offers), NOT the filtered
  // view: a drop has to work while a text filter is narrowing the list.
  //
  // A text field dropped here arrives as an unresolved catalog key rather than a series
  // name, so it simply fails the lookup: the host's catalog_key_resolver (pj_app
  // MainWindow, panel setup) goes through CatalogModel::curveDescriptor, which defaults to
  // the kPlottable capability and returns nullopt for text — and DropEventFilter then
  // passes the raw key through. A text drop and an unknown drop are therefore
  // indistinguishable here, hence one message covering both. Telling them apart needs a
  // host change, not a plugin one.
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
        items.size() > 1 ? "Selected '" + dropped + "' (one source curve at a time)" : "Selected '" + dropped + "'";
    return true;
  }

  // The Search box above the source list. Purely a view filter: it must never touch the
  // selection or the rule, so that narrowing the list cannot silently change what Apply
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
      return true;  // re-render to enable/disable the "all datasets" sub-option
    }
    if (name == "all_datasets_marker") {
      all_datasets_ = checked;
    }
    return false;
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
      // A loaded rule counts as hand-authored: without this, the natural "load a rule,
      // then pick a source" flow would have the source click overwrite the rule with the
      // active builtin template. Only reachable now that nothing is auto-selected.
      user_edited_ = true;
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

  // The dialog deliberately does NOT auto-select a source. Picking series_names_.front()
  // used to open the panel on whatever field happened to sort first — often an unusable
  // one — with the template already retargeted at it and a blank preview explaining
  // nothing. An empty selection invites a pick or a drop instead.
  //
  // A `selected_source_` restored from a layout is left alone here even when it is absent
  // from `names`: the dataset may simply not have loaded yet, and clearing it would be
  // destructive and would retarget the rule back to --SOURCE--. widget_data() reports the
  // unresolved state instead, and it heals itself once the catalog catches up.
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

  // Runs the current rule and returns the detected markers, rebased to the preview's
  // X units, so the preview can overlay what Apply would publish. `error` is set to a
  // rule compile/runtime error (empty on success) so the dialog can surface it.
  using MarkerProvider = std::function<std::vector<PJ::ChartMarker>(
      const std::string& code, const std::string& source, std::string& error)>;
  void setMarkerProvider(MarkerProvider cb) {
    marker_provider_ = std::move(cb);
  }

  void setOnTick(std::function<void()> cb) {
    on_tick_ = std::move(cb);
  }

  // The source curve currently selected in the panel — read by the toolbox's tick
  // handler so it can live-refresh just that one series while streaming.
  const std::string& selectedSource() const {
    return selected_source_;
  }

 private:
  // Is the current selection something the host can actually run a rule against? False
  // for "nothing picked" and for a layout-restored name the catalog no longer offers.
  [[nodiscard]] bool sourceResolved() const {
    return !selected_source_.empty() &&
           std::find(series_names_.begin(), series_names_.end(), selected_source_) != series_names_.end();
  }

  // Take `name` as the source, retargeting a builtin template at it. Shared by the list
  // click and the drop so the two paths cannot drift apart.
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

  // Case-insensitive substring match on the whitespace-trimmed query, preserving catalog
  // order. Recomputed only when the list or the query changed — widget_data() runs at
  // ~20 Hz and the source list can be thousands of names.
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

  // Write the current rule (script + source) to the chosen path as a portable JSON
  // file. The Save-as picker already let the user create/select the file; the host
  // guarantees the ".json" extension via the save-file-picker default_suffix contract
  // (setSaveFilePicker(..., "json")), so no manual extension handling is needed here.
  std::string saveRuleTo(std::string path) const {
    if (path.empty()) {
      return "Error: no file selected";
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
  std::string status_ = "Pick a source curve (or drag & drop one), pick a function, then Apply.";
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
  bool all_datasets_ = false;  // global marker spans every dataset (only meaningful with global_)
  std::vector<std::string> series_names_;
  RunCallback run_callback_;
  PreviewProvider preview_provider_;
  MarkerProvider marker_provider_;
  std::function<void()> on_tick_;
};

// ---------------------------------------------------------------------------
// AnomalyDetectorToolbox — reads series, runs the rule (anomaly_core), publishes markers.
// ---------------------------------------------------------------------------

// Stable plugin-local id for the single live preview generator (created EPHEMERAL,
// torn down with remove()). The host namespaces it under this plugin.
constexpr std::string_view kPreviewId = "preview";

// The toolbox IS the preview's `PreviewBackend`: the shared `EphemeralPreview` calls
// back into submitPreview/readPreviewOverlay/removePreview (the only marker-specific part
// of the preview — which host service, and how the output renders).
class AnomalyDetectorToolbox : public PJ::ToolboxPluginBase, public toolbox_preview::PreviewBackend {
 public:
  // Tear down the live preview's ephemeral object topic on unload. The host (and its
  // markers service) outlives the plugin instance during teardown, so this is safe; a
  // torn-down/unbound service simply yields no view and the call is skipped.
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
    // Refresh the catalog on every panel tick (cheap unless the structure changed) so a
    // dataset loaded after the panel opened appears in the source list without reopening,
    // then live-refresh the selected source's samples so the streaming preview follows.
    dialog_.setOnTick([this]() { onTick(); });
    onTick();  // initial fill
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
  // Panel tick (~20 Hz): keep the source list current when the catalog STRUCTURE
  // changes, and live-refresh the selected source's samples so the streaming preview
  // follows the window. Both are cheap no-ops when nothing changed.
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

  // --- toolbox_preview::PreviewBackend (the marker-specific "what it generates / where") ---

  // Submit/replace the ephemeral marker generator (kind=markers, EPHEMERAL): the host
  // runs the rule, auto-names a preview marker object topic, and returns it. An empty
  // string means "not available right now" (host service unbound) — retried next tick.
  PJ::Expected<std::string> submitPreview(const std::string& code) override {
    auto gens = services().get<PJ::sdk::DataProcessorsHostService>();
    if (!gens) {
      return std::string{};
    }
    // Declare ONLY the series the rule reads (parsed from its series("...") calls), not the
    // whole catalog: the host materializes every declared input whole-series on each run, so
    // declaring all N would re-materialize the entire dataset on the GUI thread (the freeze).
    // Built here (only on a re-submit), not every tick.
    const std::vector<std::string> refs = parseSeriesRefs(code);
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

  // Tear down the ephemeral preview generator (dialog/toolbox teardown).
  void removePreview() override {
    if (auto gens = services().get<PJ::sdk::DataProcessorsHostService>()) {
      (void)gens->remove(kPreviewId);
    }
  }

  // Submit the rule to the HOST as a marker data processor (pj.data_processors.v1, kind=markers).
  // The host owns execution: it runs the script over the requested inputs, publishes the
  // resulting PlotMarkers, and re-runs them when data changes — so markers recompute live
  // and survive this plugin's unload. The live preview is ALSO host-driven (an ephemeral
  // generator via createMarkers(..., EPHEMERAL), read back through the object
  // store), so the plugin runs NO Lua at all; the headless anomaly_runner keeps its own
  // engine, and all three run the same rule identically ("GUI == headless").
  //
  // `all_datasets` (only meaningful with `global`) publishes the global markers across
  // EVERY loaded dataset; otherwise on the active dataset only.
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
      return "Error: select a source curve, or tick Global marker";
    }
    const std::string target = global ? std::string(PJ::sdk::kGlobalMarkerTopic) : source;

    // Declare ONLY the series the rule reads (see parseSeriesRefs), same as the preview, so
    // Apply and preview stay identical AND the host doesn't materialize the whole dataset
    // whole-series on every run/commit-recompute.
    const std::vector<std::string> refs = parseSeriesRefs(code);
    std::vector<std::string_view> inputs(refs.begin(), refs.end());

    // {"scope":"all"} tells the host to publish a global marker across every dataset.
    const bool global_all = global && all_datasets;
    const std::string params = global_all ? R"({"scope":"all"})" : "{}";

    // Stable per-target id: re-Save upserts (replaces) the generator for that target.
    const std::string id = "rule/" + target;
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
  // Generic "preview a plot" engine, shared with other generator toolboxes: the source
  // list + cached samples + streaming live-refresh, and the change-gated ephemeral
  // generator submit/read-back. Only the marker-specific render (this class's
  // PreviewBackend overrides) stays here.
  toolbox_preview::SeriesCatalog catalog_;
  toolbox_preview::EphemeralPreview preview_;
  std::string preview_error_;  // last preview rule error (empty on success), shown in the status
};

}  // namespace

PJ_TOOLBOX_PLUGIN(AnomalyDetectorToolbox, kAnomalyDetectorManifest)
PJ_DIALOG_PLUGIN(AnomalyDetectorDialog)
