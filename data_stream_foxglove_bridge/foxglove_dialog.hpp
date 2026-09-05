#pragma once

#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/endpoint.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <pj_plugins/sdk/streaming_dialog.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

#include "foxglove_client_ui.hpp"
#include "foxglove_manifest.hpp"
#include "foxglove_protocol.hpp"

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
  ~FoxgloveDialog() override {
    disconnect();
  }

  /// Transfer ownership of the live socket to the caller (source plugin).
  std::unique_ptr<ix::WebSocket> takeSocket() {
    connected_ = false;
    return std::move(socket_);
  }

  /// Full catalog of channels discovered so far during this dialog's connection
  /// session (thread-safe snapshot), regardless of what the user selected. When
  /// the source steals an already-open socket (see takeSocket()), the server's
  /// one-time "advertise" burst has already happened and will not repeat — this
  /// is the only way the source learns about topics the user did NOT select, so
  /// it can advertise the full set to the host instead of just the selection.
  std::vector<PJ::FoxgloveProtocol::ChannelInfo> allDiscoveredChannels() const {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    std::vector<PJ::FoxgloveProtocol::ChannelInfo> out;
    out.reserve(channels_.size());
    for (const auto& ch : channels_) {
      out.push_back({ch.id, ch.topic, ch.encoding, ch.schema_name, ch.schema, ch.schema_encoding});
    }
    return out;
  }

  // --- Dialog protocol ---

  std::string manifest() const override {
    return kFoxgloveManifest;
  }

  std::string ui_content() const override {
    return kFoxgloveClientUi;
  }

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
    wd.setTableHeaders("topicsList", {"Topic Name", "DataType", "Encoding"});
    std::vector<std::vector<std::string>> rows;
    {
      std::lock_guard<std::mutex> lock(channels_mutex_);
      rows.reserve(channels_.size());
      for (const auto& ch : channels_) {
        if (matchesFilter(ch)) {
          rows.push_back({ch.topic, ch.schema_name, ch.schema_encoding});
        }
      }
    }
    wd.setTableRows("topicsList", rows);

    if (!selected_topic_names_.empty()) {
      wd.setSelectedItems("topicsList", selected_topic_names_);
    }

    // OK button: connected-only. The channel selection below is no longer a
    // subscribe requirement — see the "channels" config key's dual role in
    // FoxgloveSource (buildAvailableTopics): on a host that supports demand
    // subscriptions it becomes an OPTIONAL advertise filter (empty = advertise
    // everything); on a legacy host it keeps its original meaning, the
    // subscribe list.
    wd.setOkEnabled(connected_);

    return wd.toJson();
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "lineEditAddress") {
      address_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditPort") {
      if (const auto port = PJ::sdk::parsePort(text)) {
        port_ = *port;
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
      // The host emits topic names only for the rows currently visible under
      // the active filter. Preserve selections of channels filtered out of
      // view; otherwise changing the visible selection while a filter hides a
      // previously selected topic would silently drop it.
      {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        selected_topic_names_ = PJ::sdk::mergeVisibleSelection(
            selected_topic_names_, selected,
            [this](const std::string& name) {
              return std::any_of(channels_.begin(), channels_.end(), [&](const auto& channel) {
                return channel.topic == name && matchesFilter(channel);
              });
            },
            [](const std::string&) { return true; });
      }
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
  void onRejected() override {
    disconnect();
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["address"] = address_;
    cfg["port"] = port_;
    PJ::sdk::arrayLimitToJson(cfg, static_cast<uint32_t>(max_array_size_), clamp_large_arrays_);
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
    if (cfg.is_discarded()) {
      return false;
    }
    address_ = cfg.value("address", std::string("localhost"));
    port_ = cfg.value("port", 8765);
    const auto array_limit = PJ::sdk::arrayLimitFromJson(cfg);
    max_array_size_ = static_cast<int>(array_limit.max_size);
    clamp_large_arrays_ = array_limit.clamp();
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
  /// Case-insensitive substring match of the active filter against the topic
  /// name and datatype. An empty filter matches everything.
  bool matchesFilter(const DiscoveredChannel& ch) const {
    if (filter_.empty()) {
      return true;
    }
    const std::string lowered_filter = PJ::sdk::lowerAscii(filter_);
    return PJ::sdk::lowerAscii(ch.topic).find(lowered_filter) != std::string::npos ||
           PJ::sdk::lowerAscii(ch.schema_name).find(lowered_filter) != std::string::npos;
  }

  void connectToServer() {
    socket_ = std::make_unique<ix::WebSocket>();
    socket_->setUrl(PJ::sdk::composeEndpoint("ws", address_, static_cast<uint16_t>(port_)));
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
    if (json.is_discarded() || !json.is_object()) {
      return;
    }

    std::string op = json.value("op", "");
    if (op != "advertise") {
      return;
    }

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
  int max_array_size_ = 500;
  bool clamp_large_arrays_ = true;
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
