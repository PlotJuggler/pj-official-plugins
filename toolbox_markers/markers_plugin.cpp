// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// Plot Markers producer toolbox. The user drags timeseries into a list (or ticks
// "whole dataset" and picks a target), then builds an annotation set on an editable
// MarkerTimeline: each "Add time range" drops a resizable Region span, each "Add
// time event" drops a single-point Event; both are dragged / resized / right-click-
// deleted directly on the strip. Nothing reaches the plots until Save, which
// publishes the whole set as serialized PlotMarkers objects through the generic
// toolbox object-write surface (registerObjectTopicOnDataset + pushOwnedObject),
// one object topic per (dataset, scope):
//   - per-series: topic key = the dropped "topic_name/field_name" path (built with
//     sdk::markerSeriesKey, exactly as the overlay keys per-series markers), on the
//     dataset that series belongs to;
//   - global:     topic key = sdk::kGlobalMarkerTopic, on the chosen dataset(s).
// Dropping a series whose topic already carries markers loads them (dataset-scoped)
// back onto the timeline for editing. The producer owns the marker set per
// (dataset, topic) and republishes the WHOLE set on Save / Clear (last-writer-wins);
// each marker topic is capped to keep only the latest snapshot.
//
// Scope note: one shared timeline draft is published to EVERY listed target. This
// tool authors one set of marks and addresses it to each listed series; it does not
// author *different* marks per series in a single Save (that would be a per-series
// editor, intentionally out of scope here).

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <pj_base/builtin/plot_markers.hpp>
#include <pj_base/builtin/plot_markers_codec.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_base/span.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "markers_dialog_ui.hpp"
#include "markers_manifest.hpp"

namespace {

// Opaque metadata tagging the object topic as a marker set.
constexpr const char* kMarkerMetadata = R"({"object_type":"plot_markers"})";

// The MarkerTimeline works in integer step units [0, kSliderSteps]; the dialog maps
// them onto the dataset's absolute [t_min, t_max] nanosecond window for display.
constexpr int kSliderSteps = 1000;

// One staged marker, stored in ABSOLUTE nanoseconds (not step units) so a change to
// the dataset time window mid-edit cannot shift already-placed marks. The timeline
// widget speaks steps; the dialog converts ns⇆steps against the current window.
struct DraftMark {
  int id = 0;
  bool region = true;
  std::int64_t start_ns = 0;
  std::int64_t end_ns = 0;
};

// MarkersDialog — UI-only state (dropped series + selection, the staged ns draft,
// scope, the global target choice, the dataset time window). Host-side data-plane
// work (catalog read, object read/publish) lives in MarkersToolbox, reached through
// the callbacks below.
class MarkersDialog : public PJ::DialogPluginTyped {
 public:
  [[nodiscard]] std::string manifest() const override {
    return R"({"name":"Plot Markers","version":"1.0.0"})";
  }
  [[nodiscard]] std::string ui_content() const override {
    return kMarkersDialogUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;
    wd.setDropTarget("inputFrame");

    std::vector<std::vector<std::string>> rows;
    rows.reserve(series_.size());
    for (const auto& s : series_) {
      rows.push_back({s});
    }
    wd.setTableHeaders("tableSeries", std::vector<std::string>{"Series"});
    wd.setTableRows("tableSeries", rows);
    wd.setButtonIconNamed("delete_series", "trash");  // themed trash glyph, icon-only

    // Global target combo: index 0 = "All datasets", then one entry per dataset.
    std::vector<std::string> combo;
    combo.reserve(dataset_names_.size() + 1);
    combo.emplace_back("All datasets");
    for (const auto& n : dataset_names_) {
      combo.push_back(n);
    }
    wd.setItems("dataset_combo", combo);
    wd.setCurrentIndex("dataset_combo", dataset_index_);
    wd.setEnabled("dataset_combo", global_);

    // Project the ns draft onto the current window for the step-based widget.
    std::vector<PJ::TimelineMark> tl;
    tl.reserve(draft_.size());
    for (const auto& d : draft_) {
      PJ::TimelineMark m;
      m.id = d.id;
      m.region = d.region;
      m.start = nsToStep(d.start_ns);
      m.end = d.region ? nsToStep(d.end_ns) : m.start;
      tl.push_back(m);
    }
    wd.setMarkerTimelineBounds("markers_timeline", 0, kSliderSteps);
    wd.setMarkerTimelineTimeSpan("markers_timeline", t_min_ns_, t_max_ns_);
    wd.setMarkerTimelineMarks("markers_timeline", tl);

    wd.setChecked("apply_global", global_);
    wd.setEnabled("tableSeries", !global_);
    wd.setText("status_label", status_);
    return wd.toJson();
  }

  bool onItemsDropped(std::string_view widget_name, const std::vector<std::string>& items) override {
    if (widget_name != "inputFrame") {
      return false;
    }
    bool changed = false;
    for (const auto& path : items) {
      if (std::find(series_.begin(), series_.end(), path) != series_.end()) {
        continue;
      }
      series_.push_back(path);
      changed = true;
      // Surface a freshly dropped series' already-published markers for editing
      // (merged into the draft; exact duplicates are skipped).
      if (on_load_marks_) {
        loadDraft(on_load_marks_(path));
      }
    }
    return changed;
  }

  bool onSelectionChanged(std::string_view name, const std::vector<std::string>& selected) override {
    if (name == "tableSeries") {
      selected_series_ = selected;
    }
    return false;
  }

  bool onMarkerTimelineChanged(std::string_view name, const std::vector<PJ::TimelineMark>& marks) override {
    if (name != "markers_timeline") {
      return false;
    }
    std::vector<DraftMark> next;
    next.reserve(marks.size());
    for (const auto& m : marks) {
      DraftMark d;
      d.id = m.id;
      d.region = m.region;
      d.start_ns = stepToNs(m.start);
      d.end_ns = m.region ? stepToNs(m.end) : d.start_ns;
      next.push_back(d);
      next_id_ = std::max(next_id_, m.id + 1);
    }
    draft_ = std::move(next);
    return false;  // user-driven; no widget_data re-read (avoids fighting the drag)
  }

  bool onIndexChanged(std::string_view name, int index) override {
    if (name == "dataset_combo") {
      dataset_index_ = index;
    }
    return false;
  }

  bool onToggled(std::string_view name, bool checked) override {
    if (name == "apply_global") {
      global_ = checked;
      return true;  // toggles tableSeries / combo enablement → refresh
    }
    return false;
  }

  bool onClicked(std::string_view name) override {
    if (name == "add_range_btn") {
      addTimelineMark(/*region=*/true);
      return true;
    }
    if (name == "add_event_btn") {
      addTimelineMark(/*region=*/false);
      return true;
    }
    if (name == "delete_series") {
      return removeSelectedSeries();
    }
    if (name == "save_btn") {
      if (on_save_) {
        on_save_();
      }
      return true;
    }
    if (name == "clear_markers") {
      draft_.clear();
      if (on_clear_) {
        on_clear_();
      }
      return true;
    }
    return false;
  }

  bool onTick() override {
    if (on_tick_) {
      on_tick_();
    }
    const bool dirty = dirty_;
    dirty_ = false;
    return dirty;
  }

  void setStatus(std::string status) {
    status_ = std::move(status);
  }
  void setTimeSpan(std::int64_t min_ns, std::int64_t max_ns) {
    if (min_ns == t_min_ns_ && max_ns == t_max_ns_) {
      return;
    }
    t_min_ns_ = min_ns;
    t_max_ns_ = max_ns;
    dirty_ = true;  // marks are in ns, so they stay put — only the step projection refreshes
  }
  void setDatasetNames(std::vector<std::string> names) {
    if (names == dataset_names_) {
      return;
    }
    dataset_names_ = std::move(names);
    if (dataset_index_ > static_cast<int>(dataset_names_.size())) {
      dataset_index_ = 0;
    }
    dirty_ = true;
  }

  [[nodiscard]] const std::vector<std::string>& series() const {
    return series_;
  }
  [[nodiscard]] const std::vector<DraftMark>& draft() const {
    return draft_;
  }
  [[nodiscard]] bool global() const {
    return global_;
  }
  [[nodiscard]] int datasetIndex() const {
    return dataset_index_;
  }

  void setOnSave(std::function<void()> cb) {
    on_save_ = std::move(cb);
  }
  void setOnClear(std::function<void()> cb) {
    on_clear_ = std::move(cb);
  }
  void setOnLoadMarks(std::function<std::vector<DraftMark>(const std::string&)> cb) {
    on_load_marks_ = std::move(cb);
  }
  void setOnTick(std::function<void()> cb) {
    on_tick_ = std::move(cb);
  }

 private:
  // Window-relative conversions. With no data (t_max <= t_min) marks collapse to
  // t_min / step 0 — harmless, since Save is blocked until data is loaded.
  [[nodiscard]] int nsToStep(std::int64_t ns) const {
    if (t_max_ns_ <= t_min_ns_) {
      return 0;
    }
    const double frac = static_cast<double>(ns - t_min_ns_) / static_cast<double>(t_max_ns_ - t_min_ns_);
    const int step = static_cast<int>(frac * kSliderSteps + 0.5);
    return std::max(0, std::min(kSliderSteps, step));
  }
  [[nodiscard]] std::int64_t stepToNs(int step) const {
    if (t_max_ns_ <= t_min_ns_) {
      return t_min_ns_;
    }
    const double frac = static_cast<double>(step) / static_cast<double>(kSliderSteps);
    return t_min_ns_ + static_cast<std::int64_t>(frac * static_cast<double>(t_max_ns_ - t_min_ns_));
  }

  // Append a default mark (placed via the current window) for the user to drag.
  // Successive adds are staggered so they do not stack exactly.
  void addTimelineMark(bool region) {
    DraftMark d;
    d.id = next_id_++;
    d.region = region;
    const int offset = static_cast<int>(draft_.size() % 6) * 60;
    if (region) {
      const int s = std::min(300 + offset, kSliderSteps - 150);
      d.start_ns = stepToNs(s);
      d.end_ns = stepToNs(s + 150);
    } else {
      d.start_ns = stepToNs(std::min(400 + offset, kSliderSteps));
      d.end_ns = d.start_ns;
    }
    draft_.push_back(d);
  }

  // Merge externally-loaded marks into the draft, assigning fresh ids and skipping
  // exact duplicates (same kind + same ns span) so re-dropping a shared topic does
  // not pile up copies.
  void loadDraft(const std::vector<DraftMark>& loaded) {
    for (DraftMark d : loaded) {
      const bool dup = std::any_of(draft_.begin(), draft_.end(), [&](const DraftMark& e) {
        return e.region == d.region && e.start_ns == d.start_ns && e.end_ns == d.end_ns;
      });
      if (dup) {
        continue;
      }
      d.id = next_id_++;
      draft_.push_back(d);
    }
  }

  bool removeSelectedSeries() {
    if (selected_series_.empty()) {
      return false;
    }
    const auto is_selected = [this](const std::string& s) {
      return std::find(selected_series_.begin(), selected_series_.end(), s) != selected_series_.end();
    };
    const auto before = series_.size();
    series_.erase(std::remove_if(series_.begin(), series_.end(), is_selected), series_.end());
    selected_series_.clear();
    return series_.size() != before;
  }

  std::string status_ = "Load data, then add markers and press Save.";
  std::vector<std::string> series_;
  std::vector<std::string> selected_series_;
  std::vector<std::string> dataset_names_;
  std::vector<DraftMark> draft_;  // staged marks (absolute ns) shown on the timeline
  int next_id_ = 1;
  int dataset_index_ = 0;
  bool global_ = false;
  std::int64_t t_min_ns_ = 0;
  std::int64_t t_max_ns_ = 0;
  bool dirty_ = false;

  std::function<void()> on_save_;
  std::function<void()> on_clear_;
  std::function<std::vector<DraftMark>(const std::string&)> on_load_marks_;
  std::function<void()> on_tick_;
};

class MarkersToolbox : public PJ::ToolboxPluginBase {
 public:
  [[nodiscard]] uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    if (!callbacks_wired_) {
      dialog_.setOnSave([this]() { saveMarkers(); });
      dialog_.setOnClear([this]() { clearMarkers(); });
      dialog_.setOnLoadMarks([this](const std::string& path) { return loadExistingMarks(path); });
      dialog_.setOnTick([this]() { refreshCatalog(); });
      callbacks_wired_ = true;
    }
    refreshCatalog();
    return PJ::borrowDialog(dialog_);
  }

  PJ::Status bind(PJ::sdk::ServiceRegistry services) override {
    auto status = ToolboxPluginBase::bind(services);
    if (!status) {
      return status;
    }
    refreshCatalog();
    return PJ::okStatus();
  }

 private:
  using MarkerKey = std::pair<PJ::DatasetId, std::string>;  // (dataset, topic key)

  void refreshCatalog() {
    if (!toolboxHostBound()) {
      return;
    }
    auto host = toolboxHost();
    auto catalog = host.catalogSnapshot();
    if (!catalog) {
      return;
    }

    dataset_ids_.clear();
    std::vector<std::string> names;
    for (const auto& ds : catalog->dataSources()) {
      dataset_ids_.push_back(ds.handle.id);
      names.emplace_back(PJ::sdk::toStringView(ds.name));
    }
    dialog_.setDatasetNames(names);

    const auto topics = catalog->topics();
    const auto fields = catalog->fields();
    bool any = false;
    std::int64_t t_min = 0;
    std::int64_t t_max = 0;
    for (const auto& topic : topics) {
      if (topic.field_count == 0) {
        continue;
      }
      auto series = host.readSeries(fields[topic.first_field].handle);
      if (!series) {
        continue;
      }
      const auto ts = series->timestamps();
      if (ts.size() == 0) {
        continue;
      }
      const std::int64_t a = ts[0];
      const std::int64_t b = ts[ts.size() - 1];
      if (!any) {
        t_min = a;
        t_max = b;
        any = true;
      } else {
        t_min = std::min(t_min, a);
        t_max = std::max(t_max, b);
      }
    }
    if (any) {
      dialog_.setTimeSpan(t_min, t_max);
      has_window_ = true;
    }
  }

  // Build the "topic/field" -> owning dataset index used to publish per-series
  // markers. The key is built with sdk::markerSeriesKey so it matches the dropped
  // path (which the host resolves the same way) — otherwise the lookup misses and
  // per-series markers land on the wrong dataset.
  void ensureSeriesDatasetIndex() {
    if (!toolboxHostBound()) {
      return;
    }
    auto catalog = toolboxHost().catalogSnapshot();
    if (!catalog) {
      return;
    }
    series_dataset_.clear();
    const auto fields = catalog->fields();
    for (const auto& topic : catalog->topics()) {
      const std::string topic_name(PJ::sdk::toStringView(topic.name));
      for (uint32_t fi = 0; fi < topic.field_count; ++fi) {
        const auto& field = fields[topic.first_field + fi];
        series_dataset_[PJ::sdk::markerSeriesKey(topic_name, PJ::sdk::toStringView(field.name))] = topic.source.id;
      }
    }
  }

  // Owning dataset of a dropped per-series path (default: first dataset).
  [[nodiscard]] PJ::DatasetId datasetForSeries(const std::string& path) const {
    PJ::DatasetId did = dataset_ids_.empty() ? PJ::DatasetId{0} : dataset_ids_.front();
    if (const auto it = series_dataset_.find(path); it != series_dataset_.end()) {
      did = it->second;
    }
    return did;
  }

  [[nodiscard]] std::vector<MarkerKey> resolveTargets() const {
    std::vector<MarkerKey> targets;
    if (dialog_.global()) {
      const std::string key(PJ::sdk::kGlobalMarkerTopic);
      const int idx = dialog_.datasetIndex();
      if (idx <= 0 || idx > static_cast<int>(dataset_ids_.size())) {
        for (const PJ::DatasetId did : dataset_ids_) {
          targets.emplace_back(did, key);
        }
      } else {
        targets.emplace_back(dataset_ids_[static_cast<std::size_t>(idx - 1)], key);
      }
      return targets;
    }
    for (const auto& path : dialog_.series()) {
      targets.emplace_back(datasetForSeries(path), path);
    }
    return targets;
  }

  [[nodiscard]] static PJ::sdk::PlotMarker toPlotMarker(const DraftMark& d) {
    PJ::sdk::PlotMarker marker;
    marker.category = "user";
    if (d.region) {
      marker.kind = PJ::sdk::MarkerKind::kRegion;
      marker.t_start = std::min(d.start_ns, d.end_ns);
      marker.t_end = std::max(d.start_ns, d.end_ns);
      marker.severity = PJ::sdk::MarkerSeverity::kWarning;
      marker.label = "region";
    } else {
      marker.kind = PJ::sdk::MarkerKind::kEvent;
      marker.t_start = d.start_ns;
      marker.severity = PJ::sdk::MarkerSeverity::kError;
      marker.label = "event";
    }
    return marker;
  }

  // Save: publish the staged draft as the whole marker set for every target.
  void saveMarkers() {
    if (!toolboxHostBound()) {
      dialog_.setStatus("toolbox host not bound");
      return;
    }
    refreshCatalog();
    if (!has_window_) {
      dialog_.setStatus("no data loaded");
      return;
    }
    if (!dialog_.global()) {
      ensureSeriesDatasetIndex();
    }
    const std::vector<MarkerKey> targets = resolveTargets();
    if (targets.empty()) {
      dialog_.setStatus(
          dialog_.global() ? "no dataset to target" : "drag a series into the list first (or tick 'whole dataset')");
      return;
    }

    std::vector<PJ::sdk::PlotMarker> built;
    built.reserve(dialog_.draft().size());
    for (const auto& d : dialog_.draft()) {
      built.push_back(toPlotMarker(d));
    }

    for (const auto& target : targets) {
      markers_[target] = built;  // last-writer: the draft IS the whole set
      publish(target);
    }
    dialog_.setStatus(
        "saved " + std::to_string(built.size()) + " marker(s) to " + std::to_string(targets.size()) + " topic(s)");
  }

  // Clear: republish an empty set on every marker topic this toolbox knows about —
  // the currently listed targets PLUS any topic it ever wrote this session — so the
  // overlay drops them, then forget the sets.
  void clearMarkers() {
    if (!toolboxHostBound()) {
      return;
    }
    if (!dialog_.global()) {
      ensureSeriesDatasetIndex();
    }
    std::set<MarkerKey> all;
    for (const auto& t : resolveTargets()) {
      all.insert(t);
    }
    for (const auto& [key, set] : markers_) {
      all.insert(key);
    }
    for (const auto& key : all) {
      markers_[key].clear();
      publish(key);
    }
    markers_.clear();
    dialog_.setStatus("cleared");
  }

  // Read a series' currently-published markers back as draft marks (ns), so dropping
  // a topic that already carries markers shows them for editing. Dataset-scoped, so
  // it never reads the wrong dataset's topic. Best-effort: needs the object-read host.
  std::vector<DraftMark> loadExistingMarks(const std::string& path) {
    std::vector<DraftMark> out;
    const auto* reader = objectReadHost();
    if (reader == nullptr) {
      return out;
    }
    ensureSeriesDatasetIndex();
    const PJ::DatasetId did = datasetForSeries(path);
    const auto handle = reader->lookupTopicOnDataset(did, PJ::sdk::markerObjectTopicName(path));
    if (!handle) {
      return out;
    }
    const auto bytes = reader->readLatestAt(*handle, PJ::Timestamp{std::numeric_limits<std::int64_t>::max()});
    if (!bytes || bytes->empty()) {
      return out;
    }
    const PJ::Span<const uint8_t> span = bytes->view();
    const auto decoded = PJ::deserializePlotMarkers(span.data(), span.size());
    if (!decoded) {
      return out;
    }
    for (const PJ::sdk::PlotMarker& m : decoded->markers) {
      DraftMark d;
      if (m.kind == PJ::sdk::MarkerKind::kRegion) {
        d.region = true;
        d.start_ns = m.t_start;
        d.end_ns = m.t_end;
      } else if (m.kind == PJ::sdk::MarkerKind::kEvent) {
        d.region = false;
        d.start_ns = m.t_start;
        d.end_ns = m.t_start;
      } else {
        continue;  // value bands / labels are not editable on the time strip
      }
      out.push_back(d);
    }
    return out;
  }

  // Republish the producer's whole marker set for one (dataset, topic key) and cap
  // the topic to the latest snapshot so repeated Saves don't accumulate blobs.
  void publish(const MarkerKey& key) {
    auto host = toolboxHost();
    const std::string topic = PJ::sdk::markerObjectTopicName(key.second);
    auto handle = host.registerObjectTopicOnDataset(key.first, topic, kMarkerMetadata);
    if (!handle) {
      dialog_.setStatus("register failed: " + handle.error());
      return;
    }
    (void)host.setObjectTopicRetention(*handle, 1);  // keep-latest; best-effort (ignored on older host)

    PJ::sdk::PlotMarkers set;
    set.markers = markers_[key];
    const std::vector<uint8_t> bytes = PJ::serializePlotMarkers(set);
    // Sentinel timestamp 0: markers are not playback-time state; the overlay reads
    // the latest entry regardless of the cursor position.
    auto status = host.pushOwnedObject(*handle, PJ::Timestamp{0}, PJ::Span<const uint8_t>{bytes.data(), bytes.size()});
    if (!status) {
      dialog_.setStatus("push failed: " + status.error());
      return;
    }
    if (runtimeHostBound()) {
      runtimeHost().notifyDataChanged();
    }
  }

  MarkersDialog dialog_;
  bool callbacks_wired_ = false;
  bool has_window_ = false;
  std::vector<PJ::DatasetId> dataset_ids_;               // parallel to the dialog's dataset names
  std::map<std::string, PJ::DatasetId> series_dataset_;  // markerSeriesKey -> owning dataset
  std::map<MarkerKey, std::vector<PJ::sdk::PlotMarker>> markers_;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(MarkersToolbox, kMarkersManifest)
PJ_DIALOG_PLUGIN(MarkersDialog)
