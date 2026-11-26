#pragma once

/// @file ros2_dialog.hpp
/// Topic-selection dialog. It opens its own short-lived rclcpp Context and Node to poll `get_topic_names_and_types()`
/// while the user chooses, and tears them down on accept/reject; the source builds its own Context in `onStart()`.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <rclcpp/rclcpp.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../ros2_topic_subscription.hpp"
#include "datastream_ros2_ui.hpp"
#include "ros2_manifest.hpp"

namespace {

class Ros2Dialog : public PJ::DialogPluginTyped {
 public:
  ~Ros2Dialog() override {
    stopDiscovery();
  }

  std::string manifest() const override {
    return kRos2Manifest;
  }
  std::string ui_content() const override {
    return kDataStreamRos2Ui;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Selection is restored by topic name: the host matches rows by first-column text, which survives a user sort.
    auto visible = visibleRows();
    if (visible.empty()) {
      wd.setText("labelStatus", "Scanning ROS 2 topics...");
    } else {
      wd.setText("labelStatus", std::to_string(visible.size()) + " topic(s) found");
    }
    wd.setTableHeaders("listRosTopics", {"Topic", "Type"});
    PJ::AlwaysIncluded::tableToWidgets(wd, "listRosTopics", std::move(visible), selected_topics_);

    wd.setChecked("checkBoxTimestamp", use_embedded_timestamp_);
    wd.setValue("spinBoxArraySize", max_array_size_);
    wd.setChecked("radioMaxClamp", !discard_large_arrays_);
    wd.setChecked("radioMaxDiscard", discard_large_arrays_);
    wd.setChecked("checkBoxStringSuffix", remove_suffix_from_strings_);
    wd.setChecked("checkBoxStringBoolean", boolean_strings_to_number_);

    // The selection is not required (see selected_topics_'s dual role in Ros2StreamSource::buildAvailableTopics).
    wd.setOkEnabled(discovery_running_);
    return wd.toJson();
  }

  bool onTick() override {
    if (!discovery_running_) {
      return false;
    }
    // The engine ticks at ~50 ms; poll get_topic_names_and_types() once per second (PJ3 cadence).
    const auto now = std::chrono::steady_clock::now();
    if (now - last_refresh_ < std::chrono::seconds(1)) {
      return false;
    }
    last_refresh_ = now;
    refreshFromNode();
    return topics_dirty_.exchange(false);
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "lineEditFilter") {
      filter_.assign(text);
      return true;
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "checkBoxTimestamp") {
      use_embedded_timestamp_ = checked;
      return false;
    }
    if (widget_name == "radioMaxClamp") {
      if (checked) {
        discard_large_arrays_ = false;
      }
      return false;
    }
    if (widget_name == "radioMaxDiscard") {
      if (checked) {
        discard_large_arrays_ = true;
      }
      return false;
    }
    if (widget_name == "checkBoxStringSuffix") {
      remove_suffix_from_strings_ = checked;
      return false;
    }
    if (widget_name == "checkBoxStringBoolean") {
      boolean_strings_to_number_ = checked;
      return false;
    }
    return false;
  }

  using PJ::DialogPluginTyped::onValueChanged;  // keep the double overload visible
  bool onValueChanged(std::string_view widget_name, int value) override {
    if (widget_name == "spinBoxArraySize") {
      max_array_size_ = value;
      return false;
    }
    return false;
  }

  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override {
    if (widget_name != "listRosTopics") {
      return false;
    }
    // The host reports topic names only for the selectable rows visible under the filter; applyReported keeps
    // selections of filtered-out topics instead of silently dropping them.
    PJ::AlwaysIncluded::applyReported(selected_topics_, selected, visibleRows());
    return true;
  }

  bool onClicked(std::string_view widget_name) override {
    return PJ::AlwaysIncluded::applySelectButton(widget_name, selected_topics_, visibleRows());
  }

  void onAccepted(std::string_view /*json*/) override {
    stopDiscovery();
  }
  void onRejected() override {
    stopDiscovery();
  }

  std::string saveConfig() const override {
    // "selected_topics" is dual-purpose (see Ros2StreamSource::buildAvailableTopics): a legacy host subscribes exactly
    // this list; a demand-subscription host treats it as an optional advertise filter (empty = advertise everything)
    // and asks for what it wants via set_active_topics.
    nlohmann::json arr = nlohmann::json::array();
    {
      // Emit a selection only while its topic is still advertised: a restored selection for a topic no longer
      // published would be subscribed silently (no publisher, no data), has no table row so the user could not
      // deselect it, and would leak into the next session's config. Before discovery has populated, emit as-is rather
      // than drop every selection.
      std::lock_guard<std::mutex> lock(topics_mutex_);
      const bool have_discovery = !discovered_topics_.empty();
      for (const auto& name : selected_topics_) {
        if (const auto it = discovered_topics_.find(name); it != discovered_topics_.end()) {
          arr.push_back({{"name", name}, {"type", it->second}});
        } else if (!have_discovery) {
          const auto saved = saved_types_.find(name);
          arr.push_back({{"name", name}, {"type", saved == saved_types_.end() ? std::string{} : saved->second}});
        }
      }
    }
    nlohmann::json cfg;
    cfg["selected_topics"] = arr;
    // Keys mirror parser_ros's loadConfig schema; the streamer forwards this sub-object verbatim as
    // PJ_parser_binding_request_t::parser_config_json.
    nlohmann::json parser_config = {
        {"use_embedded_timestamp", use_embedded_timestamp_},
        {"boolean_strings_to_number", boolean_strings_to_number_},
        {"remove_suffix_from_strings", remove_suffix_from_strings_},
    };
    PJ::sdk::arrayLimitToJson(parser_config, static_cast<uint32_t>(max_array_size_), !discard_large_arrays_);
    cfg["parser_config"] = parser_config;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (!cfg.is_discarded()) {
      selected_topics_.clear();
      saved_types_.clear();
      if (cfg.contains("selected_topics") && cfg["selected_topics"].is_array()) {
        for (const auto& entry : cfg["selected_topics"]) {
          if (entry.is_object()) {
            const auto name = entry.value("name", std::string{});
            selected_topics_.insert(name);
            saved_types_[name] = entry.value("type", std::string{});
          }
        }
      }
      if (cfg.contains("parser_config") && cfg["parser_config"].is_object()) {
        const auto& pc = cfg["parser_config"];
        use_embedded_timestamp_ = pc.value("use_embedded_timestamp", use_embedded_timestamp_);
        const auto array_limit = PJ::sdk::arrayLimitFromJson(pc);
        max_array_size_ = static_cast<int>(array_limit.max_size);
        discard_large_arrays_ = (array_limit.policy == PJ::sdk::ArrayPolicy::kSkip);
        boolean_strings_to_number_ = pc.value("boolean_strings_to_number", boolean_strings_to_number_);
        remove_suffix_from_strings_ = pc.value("remove_suffix_from_strings", remove_suffix_from_strings_);
      }
    }
    // Discovery runs as soon as the dialog opens (PJ3 behavior); stopDiscovery() runs on accept/reject and in the
    // destructor.
    startDiscovery();
    return !cfg.is_discarded();
  }

 private:
  // Rows passing the filter, in display order. Locks topics_mutex_; callers must not hold it.
  PJ::AlwaysIncluded::TableRows visibleRows() {
    std::lock_guard<std::mutex> lock(topics_mutex_);
    return ros2_streamer::topicRows(discovered_topics_, filter_);
  }

  void startDiscovery() {
    if (discovery_running_) {
      return;
    }
    try {
      context_ = std::make_shared<rclcpp::Context>();
      context_->init(0, nullptr);
      rclcpp::NodeOptions opts;
      opts.context(context_);
      node_ = std::make_shared<rclcpp::Node>("plotjuggler_ros2_discovery", opts);
    } catch (...) {
      stopDiscovery();
      return;
    }
    discovery_running_ = true;
    last_refresh_ = std::chrono::steady_clock::now();
    refreshFromNode();
  }

  void stopDiscovery() {
    discovery_running_ = false;
    node_.reset();
    if (context_) {
      try {
        context_->shutdown("dialog closed");
      } catch (...) {}
      context_.reset();
    }
  }

  void refreshFromNode() {
    if (!node_) {
      return;
    }
    std::map<std::string, std::vector<std::string>> topics;
    try {
      topics = node_->get_topic_names_and_types();
    } catch (...) {
      return;
    }
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(topics_mutex_);
      for (const auto& [name, types] : topics) {
        if (types.empty()) {
          continue;
        }
        const auto& type = types.front();
        auto it = discovered_topics_.find(name);
        if (it == discovered_topics_.end() || it->second != type) {
          discovered_topics_[name] = type;
          changed = true;
        }
      }
    }
    if (changed) {
      topics_dirty_ = true;
    }
  }

  std::shared_ptr<rclcpp::Context> context_;
  std::shared_ptr<rclcpp::Node> node_;
  std::atomic<bool> discovery_running_{false};
  std::atomic<bool> topics_dirty_{false};
  std::chrono::steady_clock::time_point last_refresh_{};

  mutable std::mutex topics_mutex_;
  std::map<std::string, std::string> discovered_topics_;
  // What saveConfig() saves; locked rows are shown selected without being in it.
  std::set<std::string> selected_topics_;
  // The types of the loaded selection, saved back for topics not discovered yet.
  std::map<std::string, std::string> saved_types_;

  // Case-insensitive topic-name substring; empty shows everything. Not persisted.
  std::string filter_;

  // Forwarded verbatim to parser_ros via ensureParserBinding. Defaults mirror parser_ros's own, so an unsent JSON
  // behaves like sending the defaults.
  bool use_embedded_timestamp_ = false;
  int max_array_size_ = 500;
  bool discard_large_arrays_ = false;
  bool boolean_strings_to_number_ = false;
  bool remove_suffix_from_strings_ = false;
};

}  // namespace
