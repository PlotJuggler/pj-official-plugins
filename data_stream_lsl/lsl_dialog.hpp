#pragma once

#include <lsl_cpp.h>

#include <atomic>
#include <mutex>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "datastream_lsl_ui.hpp"
#include "lsl_conversions.hpp"
#include "lsl_manifest.hpp"

namespace {

/// Stream-selection dialog for the LSL source. Discovery runs on a background
/// thread (resolve_streams blocks for its wait_time, pacing the loop); onTick()
/// reports when the discovered set changed so the host re-renders. Selection is
/// restored by column-0 text (stream name), which survives a user sort of the
/// table (same contract as ros2_dialog.hpp).
class LslDialog : public PJ::DialogPluginTyped {
 public:
  ~LslDialog() override {
    stopDiscovery();
  }

  std::string manifest() const override {
    return kLslManifest;
  }
  std::string ui_content() const override {
    return kDataStreamLslUi;
  }

  std::string widget_data() override {
    ensureDiscovery();
    PJ::WidgetData wd;

    std::vector<std::vector<std::string>> rows;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      rows.reserve(discovered_.size());
      for (const auto& s : discovered_) {
        rows.push_back({s.name, s.type, std::to_string(s.channel_count), formatRate(s.srate), s.source_id});
      }
    }
    wd.setTableHeaders("tableStreams", {"Name", "Type", "Channels", "Rate (Hz)", "Source ID"});
    wd.setTableRows("tableStreams", rows);

    // Restore selection by name (column 0). Host matches first-column text, so a
    // sorted table keeps the right rows selected.
    std::vector<std::string> selected_names;
    selected_names.reserve(selected_.size());
    for (const auto& s : selected_) {
      selected_names.push_back(s.name);
    }
    wd.setSelectedItems("tableStreams", selected_names);

    wd.setChecked("radioTimestampSync", mode_ == pj_lsl::TimestampMode::kSync);
    wd.setChecked("radioTimestampRaw", mode_ == pj_lsl::TimestampMode::kRaw);
    wd.setChecked("radioTimestampReceiver", mode_ == pj_lsl::TimestampMode::kReceiver);

    wd.setOkEnabled(!selected_.empty());
    return wd.toJson();
  }

  bool onTick() override {
    ensureDiscovery();
    return dirty_.exchange(false);
  }

  bool onSelectionChanged(std::string_view widget, const std::vector<std::string>& selected) override {
    if (widget != "tableStreams") {
      return false;
    }
    // `selected` = column-0 names currently selected. Keep previously-selected
    // streams that are not currently listed (went offline) so their selection
    // survives; replace the listed portion with the new selection. Take ONE
    // consistent snapshot of discovered_ (the discovery thread can replace it
    // between accesses), and rebuild the whole selection from that snapshot.
    const bool was_empty = selected_.empty();
    std::vector<DiscoveredStream> snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      snapshot = discovered_;
    }
    std::set<std::string> listed;
    for (const auto& s : snapshot) {
      listed.insert(s.name);
    }
    std::vector<pj_lsl::SelectedStream> next;
    for (const auto& s : selected_) {
      if (listed.find(s.name) == listed.end()) {
        next.push_back(s);  // offline -> preserve
      }
    }
    // Dedupe the incoming names first: two duplicate-named rows both selected
    // report the same name twice, and expanding each occurrence would multiply
    // the matching identities (duplicate inlets/records downstream).
    const std::set<std::string> selected_names(selected.begin(), selected.end());
    for (const auto& name : selected_names) {
      for (const auto& s : snapshot) {
        if (s.name == name) {
          next.push_back({s.source_id, s.name, s.type});
        }
      }
    }
    selected_ = std::move(next);
    // Re-render when the OK-enable state (selection emptiness) flips, so the OK
    // button updates immediately rather than on the next onTick.
    return was_empty != selected_.empty();
  }

  bool onClicked(std::string_view widget) override {
    if (widget != "buttonSelectAll") {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    selected_.clear();
    for (const auto& s : discovered_) {
      selected_.push_back({s.source_id, s.name, s.type});
    }
    return true;  // re-render to reflect the new selection
  }

  bool onToggled(std::string_view widget, bool checked) override {
    if (!checked) {
      return false;
    }
    if (widget == "radioTimestampSync") {
      mode_ = pj_lsl::TimestampMode::kSync;
    } else if (widget == "radioTimestampRaw") {
      mode_ = pj_lsl::TimestampMode::kRaw;
    } else if (widget == "radioTimestampReceiver") {
      mode_ = pj_lsl::TimestampMode::kReceiver;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {
    stopDiscovery();
  }
  void onRejected() override {
    stopDiscovery();
  }

  std::string saveConfig() const override {
    return pj_lsl::serializeConfig({selected_, mode_});
  }

  bool loadConfig(std::string_view json) override {
    pj_lsl::DialogConfig cfg = pj_lsl::parseConfig(json);
    selected_ = std::move(cfg.streams);
    mode_ = cfg.mode;
    return true;
  }

 private:
  struct DiscoveredStream {
    std::string name;
    std::string type;
    std::string source_id;
    std::string uid;
    int channel_count = 0;
    double srate = 0.0;
  };

  static std::string formatRate(double srate) {
    if (srate <= 0.0) {
      return "irregular";
    }
    return std::to_string(static_cast<long>(srate + 0.5));
  }

  void ensureDiscovery() {
    if (discovery_running_.exchange(true)) {
      return;  // already running
    }
    stop_flag_ = false;
    discovery_thread_ = std::thread([this] { discoveryLoop(); });
  }

  void stopDiscovery() {
    stop_flag_ = true;
    if (discovery_thread_.joinable()) {
      discovery_thread_.join();
    }
    discovery_running_ = false;
  }

  void discoveryLoop() {
    while (!stop_flag_) {
      std::vector<lsl::stream_info> found;
      try {
        found = lsl::resolve_streams(1.0);  // blocks ~1s, pacing the loop
      } catch (...) {
        found.clear();
      }
      std::vector<DiscoveredStream> streams;
      streams.reserve(found.size());
      for (auto& info : found) {
        streams.push_back(
            {info.name(), info.type(), info.source_id(), info.uid(), info.channel_count(), info.nominal_srate()});
      }
      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sameUids(streams, discovered_)) {
          discovered_ = std::move(streams);
          changed = true;
        }
      }
      if (changed) {
        dirty_ = true;
      }
    }
  }

  static bool sameUids(const std::vector<DiscoveredStream>& a, const std::vector<DiscoveredStream>& b) {
    if (a.size() != b.size()) {
      return false;
    }
    std::set<std::string> ua;
    std::set<std::string> ub;
    for (const auto& s : a) {
      ua.insert(s.uid);
    }
    for (const auto& s : b) {
      ub.insert(s.uid);
    }
    return ua == ub;
  }

  std::mutex mutex_;
  std::vector<DiscoveredStream> discovered_;      // guarded by mutex_
  std::vector<pj_lsl::SelectedStream> selected_;  // UI thread only
  pj_lsl::TimestampMode mode_ = pj_lsl::TimestampMode::kSync;

  std::thread discovery_thread_;
  std::atomic<bool> discovery_running_{false};
  std::atomic<bool> stop_flag_{false};
  std::atomic<bool> dirty_{false};
};

}  // namespace
