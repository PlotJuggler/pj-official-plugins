// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// Plot Markers producer toolbox. The user drags timeseries into a table (or ticks
// "whole dataset" and picks a target), positions a marker with a time RangeSlider,
// and adds a shaded Region or a point Event. Markers are published as serialized
// PlotMarkers objects through the generic toolbox object-write surface
// (registerObjectTopicOnDataset + pushOwnedObject), one object topic per
// (dataset, scope):
//   - per-series: topic key = the dropped "topic_name/field_name" path, on the
//     dataset that series belongs to; the overlay (which keys per-series markers by
//     that same path) draws them only on plots showing that series;
//   - global:     topic key = sdk::kGlobalMarkerTopic, on the chosen dataset(s);
//     drawn on every plot of each target dataset.
// The producer owns the marker set per (dataset, topic) and republishes the WHOLE
// set on every change (last-writer-publish): no per-marker store mutation. This
// stands in for the future Lua scripting / detector library.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <pj_base/builtin/plot_markers.hpp>
#include <pj_base/builtin/plot_markers_codec.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_base/span.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "markers_dialog_ui.hpp"
#include "markers_manifest.hpp"

namespace {

// Opaque metadata tagging the object topic as a marker set (viewers/overlay may
// read it; the overlay locates the topic by name convention regardless).
constexpr const char* kMarkerMetadata = R"({"object_type":"plot_markers"})";

// RangeSlider works in integer units; map [0, kSliderSteps] onto the dataset's
// absolute [t_min, t_max] nanosecond window.
constexpr int kSliderSteps = 1000;

// MarkersDialog — UI-only state (dropped series, slider handles, scope, the global
// target-dataset choice, the dataset time window for the slider's time labels).
// Host-side data-plane work (catalog read, object publish) lives in MarkersToolbox
// and is reached through the std::function callbacks below.
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

    wd.setRangeSliderBounds("time_range", 0, kSliderSteps);
    wd.setRangeSliderValues("time_range", range_lower_, range_upper_);
    wd.setRangeSliderTimeSpan("time_range", t_min_ns_, t_max_ns_);

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
      if (std::find(series_.begin(), series_.end(), path) == series_.end()) {
        series_.push_back(path);
        changed = true;
      }
    }
    return changed;
  }

  bool onRangeChanged(std::string_view name, int lower, int upper) override {
    if (name != "time_range") {
      return false;
    }
    range_lower_ = lower;
    range_upper_ = upper;
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
    if (name == "add_region" && on_add_region_) {
      on_add_region_();
      return true;
    }
    if (name == "add_event" && on_add_event_) {
      on_add_event_();
      return true;
    }
    if (name == "clear_markers" && on_clear_) {
      on_clear_();
      return true;
    }
    return false;
  }

  // Periodically pull the dataset list + time window so the combo and slider stay
  // current if data is (re)loaded while the non-modal dialog is open. Only asks the
  // host to re-read widget_data when something actually changed.
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
    dirty_ = true;
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
  [[nodiscard]] bool global() const {
    return global_;
  }
  // 0 = all datasets; otherwise the (index-1)'th dataset in setDatasetNames order.
  [[nodiscard]] int datasetIndex() const {
    return dataset_index_;
  }
  [[nodiscard]] int rangeLower() const {
    return range_lower_;
  }
  [[nodiscard]] int rangeUpper() const {
    return range_upper_;
  }
  [[nodiscard]] std::pair<std::int64_t, std::int64_t> timeSpan() const {
    return {t_min_ns_, t_max_ns_};
  }

  void setOnAddRegion(std::function<void()> cb) {
    on_add_region_ = std::move(cb);
  }
  void setOnAddEvent(std::function<void()> cb) {
    on_add_event_ = std::move(cb);
  }
  void setOnClear(std::function<void()> cb) {
    on_clear_ = std::move(cb);
  }
  void setOnTick(std::function<void()> cb) {
    on_tick_ = std::move(cb);
  }

 private:
  std::string status_ = "Load data, then add markers to the plots.";
  std::vector<std::string> series_;
  std::vector<std::string> dataset_names_;
  int dataset_index_ = 0;
  bool global_ = false;
  int range_lower_ = kSliderSteps / 4;
  int range_upper_ = kSliderSteps / 2;
  std::int64_t t_min_ns_ = 0;
  std::int64_t t_max_ns_ = 0;
  bool dirty_ = false;

  std::function<void()> on_add_region_;
  std::function<void()> on_add_event_;
  std::function<void()> on_clear_;
  std::function<void()> on_tick_;
};

class MarkersToolbox : public PJ::ToolboxPluginBase {
 public:
  [[nodiscard]] uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    if (!callbacks_wired_) {
      dialog_.setOnAddRegion([this]() { addMarker(PJ::sdk::MarkerKind::kRegion); });
      dialog_.setOnAddEvent([this]() { addMarker(PJ::sdk::MarkerKind::kEvent); });
      dialog_.setOnClear([this]() { clearMarkers(); });
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

  // Refresh the dataset list (id + name) for the global selector and the union
  // [t_min, t_max] nanosecond window for the slider. Cheap enough to run each tick;
  // the per-series "topic/field" -> dataset index is built lazily at Add time
  // (ensureSeriesDatasetIndex) instead, since it is only needed to publish.
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
    }
  }

  // Build the "topic/field" -> owning dataset index used to publish per-series
  // markers. Rebuilt on demand (at Add time) rather than every tick.
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
        series_dataset_[topic_name + "/" + std::string(PJ::sdk::toStringView(field.name))] = topic.source.id;
      }
    }
  }

  [[nodiscard]] PJ::Timestamp sliderToNs(int value) const {
    const auto [t_min, t_max] = dialog_.timeSpan();
    if (t_max <= t_min) {
      return PJ::Timestamp{t_min};
    }
    const double frac = static_cast<double>(value) / static_cast<double>(kSliderSteps);
    return PJ::Timestamp{t_min + static_cast<std::int64_t>(frac * static_cast<double>(t_max - t_min))};
  }

  // Resolve the (dataset, topic-key) targets for the next marker from the current
  // scope: global → chosen dataset(s) on the "__global__" key; per-series → each
  // dropped path on its own dataset.
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
      PJ::DatasetId did = dataset_ids_.empty() ? PJ::DatasetId{0} : dataset_ids_.front();
      if (const auto it = series_dataset_.find(path); it != series_dataset_.end()) {
        did = it->second;
      }
      targets.emplace_back(did, path);
    }
    return targets;
  }

  void addMarker(PJ::sdk::MarkerKind kind) {
    if (!toolboxHostBound()) {
      dialog_.setStatus("toolbox host not bound");
      return;
    }
    refreshCatalog();
    const auto [t_min, t_max] = dialog_.timeSpan();
    if (t_max <= t_min) {
      dialog_.setStatus("no data loaded");
      return;
    }

    if (!dialog_.global()) {
      ensureSeriesDatasetIndex();  // per-series publish needs the topic/field -> dataset map
    }
    const std::vector<MarkerKey> targets = resolveTargets();
    if (targets.empty()) {
      dialog_.setStatus(
          dialog_.global() ? "no dataset to target" : "drag a series into the table first (or tick 'whole dataset')");
      return;
    }

    PJ::sdk::PlotMarker marker;
    marker.kind = kind;
    marker.category = "user";
    if (kind == PJ::sdk::MarkerKind::kRegion) {
      int lo = dialog_.rangeLower();
      int hi = dialog_.rangeUpper();
      if (hi < lo) {
        std::swap(lo, hi);
      }
      marker.t_start = sliderToNs(lo);
      marker.t_end = sliderToNs(hi);
      marker.severity = PJ::sdk::MarkerSeverity::kWarning;
      marker.label = "region";
    } else {
      marker.t_start = sliderToNs(dialog_.rangeLower());
      marker.severity = PJ::sdk::MarkerSeverity::kError;
      marker.label = "event";
    }

    for (const auto& target : targets) {
      markers_[target].push_back(marker);
      publish(target);
    }

    std::size_t total = 0;
    for (const auto& [key, set] : markers_) {
      total += set.size();
    }
    dialog_.setStatus(std::to_string(total) + " marker(s)");
  }

  void clearMarkers() {
    if (!toolboxHostBound()) {
      return;
    }
    // Republish an empty set on every (dataset, topic) we ever wrote so the overlay
    // drops them, then forget the sets.
    for (auto& [key, set] : markers_) {
      set.clear();
      publish(key);
    }
    markers_.clear();
    dialog_.setStatus("cleared");
  }

  // Republish the producer's whole marker set for one (dataset, topic key).
  // markerObjectTopicName turns the key ("__global__" or a "topic/field" path) into
  // the reserved object topic name the overlay looks up.
  void publish(const MarkerKey& key) {
    auto host = toolboxHost();
    const std::string topic = PJ::sdk::markerObjectTopicName(key.second);
    auto handle = host.registerObjectTopicOnDataset(key.first, topic, kMarkerMetadata);
    if (!handle) {
      dialog_.setStatus("register failed: " + handle.error());
      return;
    }
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
  std::vector<PJ::DatasetId> dataset_ids_;               // parallel to the dialog's dataset names
  std::map<std::string, PJ::DatasetId> series_dataset_;  // "topic/field" -> owning dataset
  // The producer's authoritative marker sets, keyed by (dataset, topic key), each
  // republished wholesale on change.
  std::map<MarkerKey, std::vector<PJ::sdk::PlotMarker>> markers_;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(MarkersToolbox, kMarkersManifest)
PJ_DIALOG_PLUGIN(MarkersDialog)
