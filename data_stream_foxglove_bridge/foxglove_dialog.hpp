#pragma once

#include <algorithm>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <pj_plugins/sdk/streaming_dialog.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <pj_websocket/connection_dialog.hpp>
#include <string>
#include <vector>

#include "foxglove_client_ui.hpp"
#include "foxglove_manifest.hpp"
#include "foxglove_protocol.hpp"

namespace {

/// Smart dialog plugin for the Foxglove Bridge streamer.
/// Owns the WebSocket connection for channel discovery during the dialog session.
/// The connection half (address or ws:// / wss:// URL, Connect / Cancel, retry with backoff,
/// status line, handoff on OK) is the one the PlotJuggler Bridge dialog uses.
/// Foxglove sends "advertise" messages automatically upon connection — no explicit
/// request needed. The dialog populates the channel table and lets the user select.
class FoxgloveDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  explicit FoxgloveDialog(PJ::WebSocket::DialogConnectionOptions options = {})
      : link_(
            options, {
                         // Foxglove sends "advertise" automatically on open — no request needed.
                         .on_open = {},
                         .on_text = [this](const std::string& text) { onServerMessage(text); },
                         .on_lost = [this] { clearChannels(); },
                     }) {}

  ~FoxgloveDialog() override {
    disconnect();
  }

  /// Transfer the connection to the caller (source plugin); its socket is null when the dialog
  /// never connected. Events not yet drained by onTick() stay queued for the new owner.
  PJ::WebSocket::Handoff takeConnection() {
    return link_.take();
  }

  /// The URL the source connects to: the configured `url` verbatim, else ws://address:port.
  std::string endpoint() const {
    return endpoint_.str();
  }

  /// Full catalog of channels discovered so far during this dialog's connection
  /// session (thread-safe snapshot), regardless of what the user selected. When
  /// the source adopts an already-open connection (see takeConnection()), the server's
  /// one-time "advertise" burst has already happened and will not repeat — this
  /// is the only way the source learns about topics the user did NOT select, so
  /// it can advertise the full set to the host instead of just the selection.
  std::vector<PJ::FoxgloveProtocol::ChannelInfo> allDiscoveredChannels() const {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    return channels_;
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

    // Connection fields. A full URL in the address field replaces address + port.
    endpoint_.toWidgets(wd, link_.idle());
    link_.toWidgets(wd);

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
    wd.setOkEnabled(link_.connected());

    return wd.toJson();
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (const auto refresh = endpoint_.onTextChanged(widget_name, text)) {
      return *refresh;
    }
    if (widget_name == "lineEditFilter") {
      filter_ = std::string(text);
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
      if (link_.idle()) {
        link_.connect(endpoint(), PJ::FoxgloveProtocol::subprotocols());
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
    return link_.tick();
  }

  void onAccepted(std::string_view /*json*/) override {
    // Do NOT disconnect — the source's onStart() adopts the open connection.
    snapshotSelectedChannels();
  }
  void onRejected() override {
    disconnect();
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    endpoint_.save(cfg);
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

  /// False for malformed JSON, a `url` that is not a ws:// / wss:// URL, or a wrong-typed field.
  bool loadConfig(std::string_view config_json) override {
    try {
      return loadConfigOrThrow(config_json);
    } catch (const nlohmann::json::exception&) {
      return false;
    }
  }

 private:
  static constexpr int kDefaultPort = 8765;  ///< foxglove_bridge's own default

  bool loadConfigOrThrow(std::string_view config_json) {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    // `url`, when present, must be a ws:// or wss:// string; it wins over address/port.
    if (!endpoint_.load(cfg, "localhost", kDefaultPort)) {
      return false;
    }
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
          PJ::FoxgloveProtocol::ChannelInfo ch;
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

  /// Case-insensitive substring match of the active filter against the topic
  /// name and datatype. An empty filter matches everything.
  bool matchesFilter(const PJ::FoxgloveProtocol::ChannelInfo& ch) const {
    if (filter_.empty()) {
      return true;
    }
    const std::string lowered_filter = PJ::sdk::lowerAscii(filter_);
    return PJ::sdk::lowerAscii(ch.topic).find(lowered_filter) != std::string::npos ||
           PJ::sdk::lowerAscii(ch.schema_name).find(lowered_filter) != std::string::npos;
  }

  void disconnect() {
    link_.disconnect();
    clearChannels();
  }

  void clearChannels() {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    channels_.clear();
  }

  void onServerMessage(const std::string& message) {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return;
    }
    if (json.value("op", "") != "advertise") {
      return;
    }

    std::lock_guard<std::mutex> lock(channels_mutex_);
    for (auto& ch : PJ::FoxgloveProtocol::parseAdvertisedChannels(json)) {
      if (ch.topic.empty() || ch.encoding.empty()) {
        continue;
      }
      // Avoid duplicates (Foxglove may re-advertise)
      const bool exists =
          std::any_of(channels_.begin(), channels_.end(), [&ch](const auto& existing) { return existing.id == ch.id; });
      if (!exists) {
        channels_.push_back(std::move(ch));
      }
    }
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
  PJ::WebSocket::Endpoint endpoint_{"localhost", kDefaultPort, {}};
  int max_array_size_ = 500;
  bool clamp_large_arrays_ = true;
  bool use_timestamp_ = false;
  std::string filter_;

  PJ::WebSocket::DialogConnection link_;

  /// Read by the source's onStart() (allDiscoveredChannels) as well as the dialog thread.
  mutable std::mutex channels_mutex_;
  std::vector<PJ::FoxgloveProtocol::ChannelInfo> channels_;
  std::vector<std::string> selected_topic_names_;
  std::vector<PJ::FoxgloveProtocol::ChannelInfo> selected_channels_snapshot_;
};

}  // namespace
