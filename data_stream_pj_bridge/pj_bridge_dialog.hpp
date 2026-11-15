#pragma once

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <pj_websocket/connection_dialog.hpp>
#include <pj_websocket/websocket_client.hpp>
#include <string>
#include <vector>

#include "pj_bridge_manifest.hpp"
#include "pj_bridge_protocol.hpp"
#include "pj_bridge_topic_subscription.hpp"
#include "websocket_client_ui.hpp"

namespace {

/// Deadlines and limits the source and the dialog enforce by polling; tests shorten them.
struct BridgeConnectionOptions {
  /// Per connection attempt, for the source and for each dialog attempt.
  std::chrono::milliseconds connect{5000};
  /// Legacy mode: how long the source waits for the subscribe response before streaming anyway.
  std::chrono::milliseconds subscribe{5000};
  /// Dialog retry backoff: first delay, doubled after each failure up to retry_max.
  std::chrono::milliseconds retry_first{1000};
  std::chrono::milliseconds retry_max{10000};
  size_t inbound_queue_bytes = PJ::WebSocket::EventQueue::kDefaultMaxBytes;

  [[nodiscard]] PJ::WebSocket::DialogConnectionOptions dialog() const {
    return {connect, retry_first, retry_max, inbound_queue_bytes};
  }
};

struct DiscoveredTopic {
  std::string name;
  std::string type;
  std::string schema_name;
  std::string schema_encoding;
  std::string schema_definition;
};

/// Smart dialog plugin for the PlotJuggler Bridge streamer.
/// Owns the WebSocket connection for topic discovery during the dialog session.
/// The dialog flow matches the original PlotJuggler plugin:
///   1. User enters address/port (or a full ws:// / wss:// URL), clicks Connect
///   2. Dialog connects via WebSocket, sends get_topics. A refused, timed-out or dropped
///      connection is retried with a capped backoff until the user cancels; the status label
///      shows connecting / retrying in N s with the last error / connected.
///   3. Topics populate the table, user selects topics
///   4. User clicks OK
///   5. saveConfig() returns the full config for the source plugin
///
/// The array-size / clamp-skip / use-timestamp parser options are not exposed
/// in the dialog (matching the PJ3 websocket client window); they keep their
/// defaults and are still emitted in saveConfig() for the source plugin.
class PjBridgeDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  explicit PjBridgeDialog(BridgeConnectionOptions options = {}) : link_(options.dialog()) {}

  ~PjBridgeDialog() override {
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

  /// Full catalog of topics discovered during this dialog's connection session
  /// (thread-safe snapshot), regardless of what the user selected. When the
  /// source adopts an already-open socket (see takeConnection()), this seeds the
  /// source's advertise catalog immediately so the host can list every topic on
  /// demand — not just the user's (now optional) filter selection — while the
  /// source's own get_topics round-trip is still in flight. `TopicInfo::encoding`
  /// carries the schema encoding and `schema` the schema text (both may be empty
  /// until the source re-fetches with include_schemas).
  std::vector<PJ::BridgeProtocol::TopicInfo> allDiscoveredTopics() const {
    std::lock_guard<std::mutex> lock(topics_mutex_);
    std::vector<PJ::BridgeProtocol::TopicInfo> out;
    out.reserve(topics_.size());
    for (const auto& t : topics_) {
      out.push_back({t.name, t.schema_encoding, t.schema_name, t.schema_definition});
    }
    return out;
  }

  // --- Dialog protocol ---

  std::string manifest() const override {
    return kPjBridgeManifest;
  }

  std::string ui_content() const override {
    return kWebSocketClientUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Connection fields. A full URL in the address field replaces address + port.
    endpoint_.toWidgets(wd, link_.idle());
    link_.toWidgets(wd);

    // Topic list — apply case-insensitive filter matching on name AND type
    wd.setTableHeaders("topicsList", {"Topic Name", "DataType"});
    std::vector<std::vector<std::string>> rows;
    {
      std::lock_guard<std::mutex> lock(topics_mutex_);
      rows.reserve(topics_.size());
      for (const auto& t : topics_) {
        if (!matchesFilter(t)) {
          continue;
        }
        rows.push_back({t.name, t.type});
      }
    }
    wd.setTableRows("topicsList", rows);
    wd.setListPlaceholder("topicsList", "Click to select, Click+Drag to multi-select");

    if (!selected_topic_names_.empty()) {
      wd.setSelectedItems("topicsList", selected_topic_names_);
    }

    // OK button: connected-only. The topic selection below is no longer a
    // subscribe requirement — see the "topics" config key's dual role in
    // PjBridgeSource: on a host that supports demand subscriptions it becomes an
    // OPTIONAL advertise filter (empty = advertise everything); on a legacy host
    // it keeps its original meaning, the subscribe list.
    wd.setOkEnabled(link_.connected());

    return wd.toJson();
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (bool refresh = false; endpoint_.onTextChanged(widget_name, text, refresh)) {
      return refresh;
    }
    if (widget_name == "lineEditFilter") {
      filter_ = std::string(text);
      return true;
    }
    return false;
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name == "buttonConnect") {
      if (link_.idle()) {
        link_.connect(endpoint());
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
      snapshotSelectedTopics();
      return true;  // re-evaluate OK button state
    }
    return false;
  }

  bool onTick() override {
    const bool changed = link_.tick({
        .on_open =
            [this] {
              tick_count_ = 0;
              requestTopics();
            },
        .on_text = [this](const std::string& text) { onServerMessage(text); },
        .on_lost = [this] { clearTopics(); },
    });

    // Periodic topic refresh (every ~1s = 20 ticks at 50ms)
    if (link_.connected() && ++tick_count_ >= 20) {
      tick_count_ = 0;
      requestTopics();
    }

    return changed;
  }

  void onAccepted(std::string_view /*json*/) override {
    // Do NOT disconnect — the source's onStart() adopts the open connection.
  }
  void onRejected() override {
    disconnect();
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    endpoint_.save(cfg);
    PJ::sdk::arrayLimitToJson(cfg, static_cast<uint32_t>(max_array_size_), clamp_large_arrays_);
    cfg["use_timestamp"] = use_timestamp_;

    // Use the snapshot — topics_ may already be cleared by disconnect()
    nlohmann::json topics_json = nlohmann::json::array();
    for (const auto& t : selected_topics_snapshot_) {
      topics_json.push_back({
          {"name", t.name},
          {"type", t.type},
          {"schema_name", t.schema_name},
          {"schema_encoding", t.schema_encoding},
          {"schema_definition", t.schema_definition},
      });
    }
    cfg["topics"] = topics_json;

    return cfg.dump();
  }

  /// False for malformed JSON or a wrong-typed field.
  bool loadConfig(std::string_view config_json) override {
    try {
      return loadConfigOrThrow(config_json);
    } catch (const nlohmann::json::exception&) {
      return false;
    }
  }

 private:
  bool loadConfigOrThrow(std::string_view config_json) {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    // `url`, when present, must be a ws:// or wss:// string; it wins over address/port.
    if (!endpoint_.load(cfg, "127.0.0.1", kDefaultPort)) {
      return false;
    }
    const auto array_limit = PJ::sdk::arrayLimitFromJson(cfg);
    max_array_size_ = static_cast<int>(array_limit.max_size);
    clamp_large_arrays_ = array_limit.clamp();
    use_timestamp_ = cfg.value("use_timestamp", false);

    // Restore previously selected topic names and snapshot for re-selection after connect
    if (cfg.contains("topics") && cfg["topics"].is_array()) {
      selected_topic_names_.clear();
      selected_topics_snapshot_.clear();
      for (const auto& t : cfg["topics"]) {
        if (t.contains("name") && t["name"].is_string()) {
          selected_topic_names_.push_back(t["name"].get<std::string>());
          DiscoveredTopic dt;
          dt.name = t.value("name", std::string{});
          dt.type = t.value("type", std::string{});
          dt.schema_name = t.value("schema_name", std::string{});
          dt.schema_encoding = t.value("schema_encoding", std::string{});
          dt.schema_definition = t.value("schema_definition", std::string{});
          selected_topics_snapshot_.push_back(std::move(dt));
        }
      }
    }
    return true;
  }

  void disconnect() {
    link_.disconnect();
    clearTopics();
  }

  void clearTopics() {
    std::lock_guard<std::mutex> lock(topics_mutex_);
    topics_.clear();
  }

  void requestTopics() {
    nlohmann::json cmd;
    cmd["command"] = "get_topics";
    cmd["protocol_version"] = 1;
    cmd["id"] = std::to_string(request_id_++);
    link_.sendText(cmd.dump());
  }

  void onServerMessage(const std::string& message) {
    auto json = nlohmann::json::parse(message, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return;
    }

    auto status = json.value("status", std::string{});
    if (status != "success") {
      return;
    }

    if (!json.contains("topics") || !json["topics"].is_array()) {
      return;
    }

    // Merge current selection with persisted selection
    std::vector<std::string> wanted = selected_topic_names_;

    std::lock_guard<std::mutex> lock(topics_mutex_);
    topics_.clear();
    for (const auto& v : json["topics"]) {
      if (!v.is_object()) {
        continue;
      }
      // stringField: a wrong-typed field of one entry degrades to empty instead of throwing
      // away the whole list.
      using PJ::BridgeProtocol::stringField;
      DiscoveredTopic dt;
      dt.name = stringField(v, "name");
      dt.type = stringField(v, "type");
      dt.schema_name = stringField(v, "schema_name");
      if (dt.schema_name.empty()) {
        dt.schema_name = dt.type;
      }
      dt.schema_encoding = stringField(v, "encoding");
      dt.schema_definition = stringField(v, "definition");
      if (!dt.name.empty()) {
        topics_.push_back(std::move(dt));
      }
    }

    // Re-apply selection: keep only names that still exist in the new topic list
    std::vector<std::string> new_selection;
    for (const auto& name : wanted) {
      for (const auto& t : topics_) {
        if (t.name == name) {
          new_selection.push_back(name);
          break;
        }
      }
    }
    selected_topic_names_ = std::move(new_selection);
  }

  /// Snapshot the full schema info for selected topics so saveConfig()
  /// doesn't depend on topics_ (which gets cleared on disconnect).
  void snapshotSelectedTopics() {
    std::lock_guard<std::mutex> lock(topics_mutex_);
    selected_topics_snapshot_.clear();
    for (const auto& name : selected_topic_names_) {
      for (const auto& t : topics_) {
        if (t.name == name) {
          selected_topics_snapshot_.push_back(t);
          break;
        }
      }
    }
  }

  /// Case-insensitive substring match on topic name and type.
  /// Selected topics always match (so they remain visible even when filtered).
  bool matchesFilter(const DiscoveredTopic& t) const {
    if (filter_.empty()) {
      return true;
    }
    // Always show selected topics regardless of filter
    for (const auto& sel : selected_topic_names_) {
      if (sel == t.name) {
        return true;
      }
    }
    // Case-insensitive search
    const std::string lower_filter = PJ::sdk::lowerAscii(filter_);
    const std::string lower_name = PJ::sdk::lowerAscii(t.name);
    const std::string lower_type = PJ::sdk::lowerAscii(t.type);
    return lower_name.find(lower_filter) != std::string::npos || lower_type.find(lower_filter) != std::string::npos;
  }

  // --- State ---
  static constexpr int kDefaultPort = 9090;  ///< the bridge server's own default
  PJ::WebSocket::Endpoint endpoint_{"127.0.0.1", kDefaultPort, {}};
  int max_array_size_ = 500;
  bool clamp_large_arrays_ = true;
  bool use_timestamp_ = false;
  std::string filter_;

  PJ::WebSocket::DialogConnection link_;

  mutable std::mutex topics_mutex_;
  std::vector<DiscoveredTopic> topics_;
  std::vector<std::string> selected_topic_names_;
  std::vector<DiscoveredTopic> selected_topics_snapshot_;
  int tick_count_ = 0;
  uint32_t request_id_ = 1;
};

}  // namespace
