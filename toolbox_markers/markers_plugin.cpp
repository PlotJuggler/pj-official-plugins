// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// Plot Markers demo producer toolbox. Reads the loaded series via the toolbox
// catalog, then creates markers through the pj.marker_store.v1 service. This is
// the "simulator" producer: it hardcodes marker creation behind dialog buttons,
// standing in for the future Lua scripting / detector library that will emit
// markers through the exact same MarkerStoreHostView API.

#include <functional>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "markers_dialog_ui.hpp"
#include "markers_manifest.hpp"

namespace {

// Reserved dataset-global marker topic (matches kGlobalMarkerTopic in the PJ4
// PlotMarkersItem overlay): markers here render on every plot of the dataset.
constexpr const char* kGlobalTopic = "__global__";

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
    wd.setText("status_label", status_);
    return wd.toJson();
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

  void setStatus(std::string status) {
    status_ = std::move(status);
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

 private:
  std::string status_ = "Load data, then add markers to the plots.";
  std::function<void()> on_add_region_;
  std::function<void()> on_add_event_;
  std::function<void()> on_clear_;
};

class MarkersToolbox : public PJ::ToolboxPluginBase {
 public:
  MarkersToolbox() {
    dialog_.setOnAddRegion([this]() { addMarker(PJ::sdk::MarkerKind::kRegion); });
    dialog_.setOnAddEvent([this]() { addMarker(PJ::sdk::MarkerKind::kEvent); });
    dialog_.setOnClear([this]() { clearMarkers(); });
  }

  [[nodiscard]] uint64_t capabilities() const override {
    return PJ::kToolboxCapabilityHasDialog | PJ::kToolboxCapabilityNonModalDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

 private:
  // Resolve the first scalar series in the catalog: its dataset (topic.source.id
  // is the DatasetId) and raw [t0, t1] time span (read from the Arrow series).
  bool firstSeries(PJ::DatasetId& dataset, PJ::Timestamp& t0, PJ::Timestamp& t1) {
    auto host = toolboxHost();
    auto catalog = host.catalogSnapshot();
    if (!catalog) {
      return false;
    }
    const auto topics = catalog->topics();
    const auto fields = catalog->fields();
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
      dataset = topic.source.id;
      t0 = ts[0];
      t1 = ts[ts.size() - 1];
      return true;
    }
    return false;
  }

  void addMarker(PJ::sdk::MarkerKind kind) {
    auto marker_host = services().get<PJ::sdk::MarkerStoreService>();
    if (!marker_host) {
      dialog_.setStatus("marker service unavailable");
      return;
    }
    PJ::DatasetId dataset = 0;
    PJ::Timestamp t0 = 0;
    PJ::Timestamp t1 = 0;
    if (!firstSeries(dataset, t0, t1)) {
      dialog_.setStatus("no data loaded");
      return;
    }
    const PJ::Timestamp span = (t1 > t0) ? (t1 - t0) : 1;

    PJ::sdk::PlotMarker marker;
    marker.kind = kind;
    if (kind == PJ::sdk::MarkerKind::kRegion) {
      marker.t_start = t0 + span / 4;
      marker.t_end = t0 + span / 2;
      marker.severity = PJ::sdk::MarkerSeverity::kWarning;
      marker.category = "demo";
      marker.label = "region";
    } else {
      marker.t_start = t0 + (span * 7) / 10;
      marker.severity = PJ::sdk::MarkerSeverity::kError;
      marker.category = "demo";
      marker.label = "event";
    }

    auto id = marker_host->add(dataset, kGlobalTopic, marker);
    dialog_.setStatus(id.has_value() ? "marker added" : ("add failed: " + id.error()));
    runtimeHost().notifyDataChanged();
  }

  void clearMarkers() {
    auto marker_host = services().get<PJ::sdk::MarkerStoreService>();
    if (!marker_host) {
      return;
    }
    PJ::DatasetId dataset = 0;
    PJ::Timestamp t0 = 0;
    PJ::Timestamp t1 = 0;
    if (!firstSeries(dataset, t0, t1)) {
      return;
    }
    if (auto markers = marker_host->query(dataset, kGlobalTopic)) {
      for (const auto& entry : *markers) {
        (void)marker_host->remove(dataset, kGlobalTopic, entry.id);
      }
    }
    dialog_.setStatus("cleared");
    runtimeHost().notifyDataChanged();
  }

  MarkersDialog dialog_;
};

}  // namespace

PJ_TOOLBOX_PLUGIN(MarkersToolbox, kMarkersManifest)
PJ_DIALOG_PLUGIN(MarkersDialog)
