#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "foxglove_client_ui.hpp"
#include "foxglove_manifest.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct DiscoveredChannel {
  uint64_t id = 0;
  std::string topic;
  std::string encoding;
  std::string schema_name;
  std::string schema;
  std::string schema_encoding;
};

/// Smart dialog plugin for the Foxglove Bridge streamer.
/// Owns the WebSocket connection for channel discovery during the dialog session.
/// Foxglove sends "advertise" messages automatically upon connection — no explicit
/// request needed. The dialog populates the channel table and lets the user select.
class FoxgloveDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  ~FoxgloveDialog() override { disconnect(); }

  /// Transfer ownership of the live socket to the caller (source plugin).
  std::unique_ptr<ix::WebSocket> takeSocket() {
    connected_ = false;
    return std::move(socket_);
  }

  // --- Dialog protocol ---

  std::string manifest() const override { return kFoxgloveManifest; }

  std::string ui_content() const override { return kFoxgloveClientUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Connection fields
    wd.setText("lineEditAddress", address_);
    wd.setText("lineEditPort", std::to_string(port_));
    wd.setEnabled("lineEditAddress", !connected_);
    wd.setEnabled("lineEditPort", !connected_);
    wd.setButtonText("buttonConnect", connected_ ? "Connected" : "Connect");
    wd.setChecked("buttonConnect", connected_.load());

    // Parser options
    wd.setValue("spinBoxArraySize", max_array_size_);
    wd.setChecked("radioClamp", clamp_large_arrays_);
    wd.setChecked("radioSkip", !clamp_large_arrays_);
    wd.setChecked("checkBoxUseTimestamp", use_timestamp_);

    // Channel list
    wd.setTableHeaders("topicsList", {"Topic Name", "DataType"});
    std::vector<std::vector<std::string>> rows;
    {
      std::lock_guard<std::mutex> lock(channels_mutex_);
      rows.reserve(channels_.size());
      for (const auto& ch : channels_) {
        // Apply filter
        if (!filter_.empty()) {
          std::string lower_topic = ch.topic;
          std::string lower_type = ch.schema_name;
          std::string lower_filter = filter_;
          for (auto& c : lower_topic) c = static_cast<char>(std::tolower(c));
          for (auto& c : lower_type) c = static_cast<char>(std::tolower(c));
          for (auto& c : lower_filter) c = static_cast<char>(std::tolower(c));
          if (lower_topic.find(lower_filter) == std::string::npos &&
              lower_type.find(lower_filter) == std::string::npos) {
            continue;
          }
        }
        rows.push_back({ch.topic, ch.schema_name});
      }
    }
    wd.setTableRows("topicsList", rows);

    if (!selected_topic_names_.empty()) {
      wd.setSelectedItems("topicsList", selected_topic_names_);
    }

    // OK button: enabled only when connected and channels are selected
    wd.setOkEnabled(connected_ && !selected_topic_names_.empty());

    return wd.toJson();
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "lineEditAddress") {
      address_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditPort") {
      auto val = std::atoi(std::string(text).c_str());
      if (val > 0 && val <= 65535) {
        port_ = val;
      }
      return false;
    }
    if (widget_name == "lineEditFilter") {
      filter_ = std::string(text);
      channels_dirty_ = true;
      return true;
    }
    return false;
  }

  bool onValueChanged(std::string_view widget_name, int value) override {
    if (widget_name == "spinBoxArraySize") {
      max_array_size_ = value;
      return false;
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "radioClamp") {
      clamp_large_arrays_ = checked;
      return false;
    }
    if (widget_name == "radioSkip") {
      clamp_large_arrays_ = !checked;
      return false;
    }
    if (widget_name == "checkBoxUseTimestamp") {
      use_timestamp_ = checked;
      return false;
    }
    return false;
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name == "buttonConnect") {
      if (!connected_) {
        connectToServer();
      } else {
        disconnect();
      }
      return true;
    }
    return false;
  }

  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override {
    if (widget_name == "topicsList") {
      selected_topic_names_ = selected;
      snapshotSelectedChannels();
      return true;
    }
    return false;
  }

  bool onTick() override {
    if (tick_dirty_) {
      tick_dirty_ = false;
      return true;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {
    // Do NOT disconnect — the source's onStart() will steal the socket.
    snapshotSelectedChannels();
  }
  void onRejected() override { disconnect(); }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["address"] = address_;
    cfg["port"] = port_;
    cfg["max_array_size"] = max_array_size_;
    cfg["clamp_large_arrays"] = clamp_large_arrays_;
    cfg["use_timestamp"] = use_timestamp_;

    // Use the snapshot — channels_ may be cleared by disconnect()
    nlohmann::json channels_json = nlohmann::json::array();
    for (const auto& ch : selected_channels_snapshot_) {
      channels_json.push_back({
          {"id", ch.id},
          {"topic", ch.topic},
          {"encoding", ch.encoding},
          {"schema_name", ch.schema_name},
          {"schema", ch.schema},
          {"schema_encoding", ch.schema_encoding},
      });
    }
    cfg["channels"] = channels_json;

    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) return false;
    address_ = cfg.value("address", std::string("localhost"));
    port_ = cfg.value("port", 8765);
    max_array_size_ = cfg.value("max_array_size", 100);
    clamp_large_arrays_ = cfg.value("clamp_large_arrays", false);
    use_timestamp_ = cfg.value("use_timestamp", false);

    // Restore previously selected topic names and snapshot
    if (cfg.contains("channels") && cfg["channels"].is_array()) {
      selected_topic_names_.clear();
      selected_channels_snapshot_.clear();
      for (const auto& ch_json : cfg["channels"]) {
        if (ch_json.contains("topic") && ch_json["topic"].is_string()) {
          selected_topic_names_.push_back(ch_json["topic"].get<std::string>());
          DiscoveredChannel ch;
          ch.id = ch_json.value("id", uint64_t{0});
          ch.topic = ch_json.value("topic", std::string{});
          ch.encoding = ch_json.value("encoding", std::string{});
          ch.schema_name = ch_json.value("schema_name", std::string{});
          ch.schema = ch_json.value("schema", std::string{});
          ch.schema_encoding = ch_json.value("schema_encoding", std::string{});
          selected_channels_snapshot_.push_back(std::move(ch));
        }
      }
    }
    return true;
  }

 private:
  void connectToServer() {
    socket_ = std::make_unique<ix::WebSocket>();
    socket_->setUrl("ws://" + address_ + ":" + std::to_string(port_));
    socket_->addSubProtocol("foxglove.sdk.v1");

    socket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
      if (msg->type == ix::WebSocketMessageType::Open) {
        connected_ = true;
        tick_dirty_ = true;
        // Foxglove sends "advertise" automatically — no request needed
      } else if (msg->type == ix::WebSocketMessageType::Close) {
        connected_ = false;
        {
          std::lock_guard<std::mutex> lock(channels_mutex_);
          channels_.clear();
        }
        channels_dirty_ = true;
        tick_dirty_ = true;
      } else if (msg->type == ix::WebSocketMessageType::Message && !msg->binary) {
        onServerMessage(msg->str);
      }
    });

    socket_->start();
  }

  void disconnect() {
    if (socket_) {
      socket_->stop();
      socket_.reset();
    }
    connected_ = false;
  }

  void onServerMessage(const std::string& message) {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return;

    std::string op = json.value("op", "");
    if (op != "advertise") return;

    auto channels_arr = json.value("channels", nlohmann::json::array());

    std::lock_guard<std::mutex> lock(channels_mutex_);
    for (const auto& ch_json : channels_arr) {
      DiscoveredChannel ch;
      ch.id = ch_json.value("id", uint64_t{0});
      ch.topic = ch_json.value("topic", "");
      ch.encoding = ch_json.value("encoding", "");
      ch.schema_name = ch_json.value("schemaName", "");
      ch.schema = ch_json.value("schema", "");
      ch.schema_encoding = ch_json.value("schemaEncoding", "");

      if (!ch.topic.empty() && !ch.encoding.empty()) {
        // Avoid duplicates (Foxglove may re-advertise)
        bool exists = false;
        for (const auto& existing : channels_) {
          if (existing.id == ch.id) {
            exists = true;
            break;
          }
        }
        if (!exists) {
          channels_.push_back(std::move(ch));
        }
      }
    }

    channels_dirty_ = true;
    tick_dirty_ = true;
  }

  void snapshotSelectedChannels() {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    selected_channels_snapshot_.clear();
    for (const auto& name : selected_topic_names_) {
      for (const auto& ch : channels_) {
        if (ch.topic == name) {
          selected_channels_snapshot_.push_back(ch);
          break;
        }
      }
    }
  }

  // --- State ---
  std::string address_ = "localhost";
  int port_ = 8765;
  int max_array_size_ = 100;
  bool clamp_large_arrays_ = false;
  bool use_timestamp_ = false;
  std::string filter_;

  std::atomic<bool> connected_ = false;
  std::unique_ptr<ix::WebSocket> socket_;

  mutable std::mutex channels_mutex_;
  std::vector<DiscoveredChannel> channels_;
  std::vector<std::string> selected_topic_names_;
  std::vector<DiscoveredChannel> selected_channels_snapshot_;
  bool channels_dirty_ = true;
  std::atomic<bool> tick_dirty_ = false;
};

}  // namespace
