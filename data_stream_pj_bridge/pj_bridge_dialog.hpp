#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "websocket_client_ui.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QWebSocket>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {

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
///   1. User enters address/port, clicks Connect
///   2. Dialog connects via WebSocket, sends get_topics
///   3. Topics populate the table, user selects topics
///   4. User configures parser options (array size, clamp/skip, use timestamp)
///   5. User clicks Subscribe (OK)
///   6. saveConfig() returns the full config for the source plugin
class PjBridgeDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  ~PjBridgeDialog() override { disconnect(); }

  // --- Dialog protocol ---

  std::string manifest() const override {
    return R"({"name":"PlotJuggler Bridge","version":"1.0.0"})";
  }

  std::string ui_content() const override { return kWebSocketClientUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Connection fields
    wd.setText("lineEditAddress", address_);
    wd.setText("lineEditPort", std::to_string(port_));
    wd.setEnabled("lineEditAddress", !connected_);
    wd.setEnabled("lineEditPort", !connected_);
    wd.setButtonText("buttonConnect", connected_ ? "Connected" : "Connect");
    wd.setChecked("buttonConnect", connected_);

    // Parser options
    wd.setValue("spinBoxArraySize", max_array_size_);
    wd.setChecked("radioClamp", clamp_large_arrays_);
    wd.setChecked("radioSkip", !clamp_large_arrays_);
    wd.setChecked("checkBoxUseTimestamp", use_timestamp_);

    // Topic list — apply case-insensitive filter matching on name AND type
    wd.setTableHeaders("topicsList", {"Topic Name", "DataType"});
    std::vector<std::vector<std::string>> rows;
    rows.reserve(topics_.size());
    for (const auto& t : topics_) {
      if (!matchesFilter(t)) continue;
      rows.push_back({t.name, t.type});
    }
    wd.setTableRows("topicsList", rows);

    if (!selected_topic_names_.empty()) {
      wd.setSelectedItems("topicsList", selected_topic_names_);
    }

    // OK button: enabled only when connected and topics are selected
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
      applyFilter();
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
      return true;  // re-evaluate OK button state
    }
    return false;
  }

  bool onTick() override {
    if (!socket_) {
      return false;
    }

    // Process pending WebSocket events
    // (QWebSocket signals fire during the dialog's event loop — onTick just checks for state changes)
    if (tick_dirty_) {
      tick_dirty_ = false;
      return true;
    }

    // Periodic topic refresh (every ~1s = 20 ticks at 50ms)
    if (connected_ && ++tick_count_ >= 20) {
      tick_count_ = 0;
      requestTopics();
    }

    return false;
  }

  void onAccepted(std::string_view /*json*/) override { disconnect(); }
  void onRejected() override { disconnect(); }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["address"] = address_;
    cfg["port"] = port_;
    cfg["max_array_size"] = max_array_size_;
    cfg["clamp_large_arrays"] = clamp_large_arrays_;
    cfg["use_timestamp"] = use_timestamp_;

    // Save selected topics with schema info for the source plugin
    nlohmann::json topics_json = nlohmann::json::array();
    for (const auto& name : selected_topic_names_) {
      for (const auto& t : topics_) {
        if (t.name == name) {
          topics_json.push_back({
              {"name", t.name},
              {"type", t.type},
              {"schema_name", t.schema_name},
              {"schema_encoding", t.schema_encoding},
              {"schema_definition", t.schema_definition},
          });
          break;
        }
      }
    }
    cfg["topics"] = topics_json;

    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) return false;
    address_ = cfg.value("address", std::string("127.0.0.1"));
    port_ = cfg.value("port", 9871);
    max_array_size_ = cfg.value("max_array_size", 100);
    clamp_large_arrays_ = cfg.value("clamp_large_arrays", false);
    use_timestamp_ = cfg.value("use_timestamp", false);

    // Restore previously selected topic names for re-selection after connect
    if (cfg.contains("topics") && cfg["topics"].is_array()) {
      selected_topic_names_.clear();
      for (const auto& t : cfg["topics"]) {
        if (t.contains("name") && t["name"].is_string()) {
          selected_topic_names_.push_back(t["name"].get<std::string>());
        }
      }
    }
    return true;
  }

 private:
  void connectToServer() {
    socket_ = std::make_unique<QWebSocket>();

    QObject::connect(socket_.get(), &QWebSocket::connected, [this]() {
      connected_ = true;
      tick_dirty_ = true;
      requestTopics();
    });

    QObject::connect(socket_.get(), &QWebSocket::disconnected, [this]() {
      connected_ = false;
      topics_.clear();
      topics_dirty_ = true;
      tick_dirty_ = true;
    });

    QObject::connect(socket_.get(), &QWebSocket::textMessageReceived, [this](const QString& msg) {
      onServerMessage(msg);
    });

    QUrl url(QString("ws://%1:%2").arg(QString::fromStdString(address_)).arg(port_));
    socket_->open(url);
  }

  void disconnect() {
    if (socket_) {
      socket_->close();
      socket_.reset();
    }
    connected_ = false;
  }

  void requestTopics() {
    if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState) {
      return;
    }
    QJsonObject cmd;
    cmd["command"] = "get_topics";
    cmd["protocol_version"] = 1;
    cmd["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    socket_->sendTextMessage(QString::fromUtf8(QJsonDocument(cmd).toJson(QJsonDocument::Compact)));
  }

  void onServerMessage(const QString& message) {
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(message.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    auto obj = doc.object();
    auto status = obj.value("status").toString();
    if (status != "success") return;

    if (!obj.contains("topics") || !obj.value("topics").isArray()) return;

    // Merge current selection with persisted selection
    std::vector<std::string> wanted = selected_topic_names_;

    topics_.clear();
    auto arr = obj.value("topics").toArray();
    for (const auto& v : arr) {
      if (!v.isObject()) continue;
      auto t = v.toObject();
      DiscoveredTopic dt;
      dt.name = t.value("name").toString().toStdString();
      dt.type = t.value("type").toString().toStdString();
      dt.schema_name = t.value("schema_name").toString(QString::fromStdString(dt.type)).toStdString();
      dt.schema_encoding = t.value("encoding").toString().toStdString();
      dt.schema_definition = t.value("definition").toString().toStdString();
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

    applyFilter();
    topics_dirty_ = true;
    tick_dirty_ = true;
  }

  void applyFilter() { topics_dirty_ = true; }

  /// Case-insensitive substring match on topic name and type.
  /// Selected topics always match (so they remain visible even when filtered).
  bool matchesFilter(const DiscoveredTopic& t) const {
    if (filter_.empty()) return true;
    // Always show selected topics regardless of filter
    for (const auto& sel : selected_topic_names_) {
      if (sel == t.name) return true;
    }
    // Case-insensitive search
    std::string lower_filter = filter_;
    std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::string lower_name = t.name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::string lower_type = t.type;
    std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower_name.find(lower_filter) != std::string::npos ||
           lower_type.find(lower_filter) != std::string::npos;
  }

  // --- State ---
  std::string address_ = "127.0.0.1";
  int port_ = 9871;
  int max_array_size_ = 100;
  bool clamp_large_arrays_ = false;
  bool use_timestamp_ = false;
  std::string filter_;

  bool connected_ = false;
  std::unique_ptr<QWebSocket> socket_;

  std::vector<DiscoveredTopic> topics_;
  std::vector<std::string> selected_topic_names_;
  bool topics_dirty_ = true;
  bool tick_dirty_ = false;
  int tick_count_ = 0;
};

}  // namespace
