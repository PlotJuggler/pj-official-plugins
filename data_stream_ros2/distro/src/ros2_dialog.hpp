#pragma once

/// @file ros2_dialog.hpp
/// @brief Topic-selection dialog for the ROS 2 streaming source.
///
/// Lifecycle: the dialog opens its own short-lived rclcpp Context and Node
/// so it can poll `get_topic_names_and_types()` while the user is choosing.
/// The Context is torn down on accept/reject — the source plugin builds its
/// own Context in `onStart()` independently.

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    std::vector<std::string> labels;
    {
      std::lock_guard<std::mutex> lock(topics_mutex_);
      labels.reserve(discovered_topics_.size());
      for (const auto& [name, type] : discovered_topics_) {
        labels.push_back(formatLabel(name, type));
      }
    }
    wd.setListItems("listWidget", labels);

    std::vector<std::string> selected_labels;
    selected_labels.reserve(selected_topics_.size());
    for (const auto& [name, type] : selected_topics_) {
      selected_labels.push_back(formatLabel(name, type));
    }
    wd.setSelectedItems("listWidget", selected_labels);

    wd.setButtonText("buttonRefresh", discovery_running_ ? "Stop discovery" : "Start discovery");

    if (discovery_running_) {
      wd.setText("labelStatus", "Discovering... " + std::to_string(labels.size()) + " topic(s) found");
    } else if (labels.empty()) {
      wd.setText("labelStatus", "Click 'Start discovery' to scan ROS 2 topics");
    } else {
      wd.setText("labelStatus", std::to_string(labels.size()) + " topic(s) found — discovery stopped");
    }

    wd.setOkEnabled(!selected_topics_.empty());
    return wd.toJson();
  }

  bool onTick() override {
    if (!discovery_running_) {
      return false;
    }
    refreshFromNode();
    return topics_dirty_.exchange(false);
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name == "buttonRefresh") {
      if (discovery_running_) {
        stopDiscovery();
      } else {
        startDiscovery();
      }
      return true;
    }
    return false;
  }

  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override {
    if (widget_name != "listWidget") {
      return false;
    }
    std::vector<std::pair<std::string, std::string>> next;
    next.reserve(selected.size());
    {
      std::lock_guard<std::mutex> lock(topics_mutex_);
      for (const auto& label : selected) {
        for (const auto& [name, type] : discovered_topics_) {
          if (formatLabel(name, type) == label) {
            next.emplace_back(name, type);
            break;
          }
        }
      }
    }
    selected_topics_ = std::move(next);
    // Re-render so widget_data() refreshes the OK-enabled state.
    return true;
  }

  void onAccepted(std::string_view /*json*/) override {
    stopDiscovery();
  }
  void onRejected() override {
    stopDiscovery();
  }

  std::string saveConfig() const override {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [name, type] : selected_topics_) {
      arr.push_back({{"name", name}, {"type", type}});
    }
    nlohmann::json cfg;
    cfg["selected_topics"] = arr;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    selected_topics_.clear();
    if (cfg.contains("selected_topics") && cfg["selected_topics"].is_array()) {
      for (const auto& entry : cfg["selected_topics"]) {
        if (entry.is_object()) {
          selected_topics_.emplace_back(entry.value("name", std::string{}), entry.value("type", std::string{}));
        }
      }
    }
    return true;
  }

 private:
  static std::string formatLabel(const std::string& name, const std::string& type) {
    return name + "  [" + type + "]";
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

  std::mutex topics_mutex_;
  std::map<std::string, std::string> discovered_topics_;
  std::vector<std::pair<std::string, std::string>> selected_topics_;
};

}  // namespace
