#pragma once

#include <mqtt/async_client.h>

#include <mutex>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/streaming_dialog.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "datastream_mqtt_ui.hpp"
#include "mqtt_connection.hpp"
#include "mqtt_manifest.hpp"

namespace {

/// Dialog plugin for the MQTT Subscriber.
/// Uses the original .ui layout with Connection + Security tabs.
/// Widget names match the original DataStreamMQTT .ui file.
class MqttDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  /// Called by the DataSource after loadConfig() to populate available encodings.
  void setAvailableEncodings(std::vector<std::string> encodings) {
    available_encodings_ = std::move(encodings);
  }

  // --- Dialog protocol ---

  std::string manifest() const override {
    return kMqttManifest;
  }

  std::string ui_content() const override {
    return kDataStreamMqttUi;
  }

  ~MqttDialog() override {
    disconnectBroker();
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Connection tab widgets
    wd.setText("lineEditHost", broker_address_);
    wd.setText("lineEditPort", std::to_string(port_));
    wd.setText("lineEditUsername", username_);
    wd.setText("lineEditPassword", password_);
    wd.setEnabled("lineEditHost", !connected_);
    wd.setEnabled("lineEditPort", !connected_);

    // Connect button state + error feedback (label_12 stays empty while idle).
    wd.setButtonText("buttonConnect", connected_ ? "Disconnect" : "Connect");
    wd.setText(
        "label_12", last_connect_error_.empty() ? (connected_ ? "Connected — select topics below" : "")
                                                : ("Connection error: " + last_connect_error_));

    // Protocol version combo
    wd.setCurrentIndex("comboBoxVersion", protocol_version_index_);

    // QoS combo
    wd.setCurrentIndex("comboBoxQoS", qos_);

    // SSL checkbox
    wd.setChecked("checkBoxSecurity", use_ssl_);

    // MQTT subscription pattern (broker-side discovery filter, applied on connect)
    wd.setText("lineEditTopicFilter", topic_filter_);

    // Discovered topic list (from live MQTT subscription), narrowed by the
    // view filter. Selection is text-keyed with the FULL selected set: the
    // host ignores names not currently listed, and onSelectionChanged merges
    // them back, so filtered-out selections survive.
    {
      std::lock_guard<std::mutex> lock(topics_mutex_);
      std::vector<std::string> topic_list;
      topic_list.reserve(discovered_topics_.size());
      for (const auto& topic : discovered_topics_) {
        if (matchesViewFilter(topic)) {
          topic_list.push_back(topic);
        }
      }
      wd.setListItems("listWidget", topic_list);
      wd.setSelectedItems("listWidget", selected_topics_);
    }

    // Protocol combo — dynamically populated from available parsers
    const bool has_encodings = PJ::sdk::writeEncodingSelector(wd, "comboBoxProtocol", available_encodings_, encoding_);

    // TLS certificate file pickers. Icon-only browse buttons (empty label, the
    // shared "contract" certificate glyph); the editable path fields double as
    // the file-picked display and a manual-entry field.
    wd.setFilePicker("buttonLoadServerCertificate", "", "*.pem *.crt *.cer", "Select Server CA Certificate");
    wd.setFilePicker("buttonLoadClientCertificate", "", "*.pem *.crt *.cer", "Select Client Certificate");
    wd.setFilePicker("buttonLoadPrivateKey", "", "*.pem *.key", "Select Private Key");
    wd.setButtonIconNamed("buttonLoadServerCertificate", "contract");
    wd.setButtonIconNamed("buttonLoadClientCertificate", "contract");
    wd.setButtonIconNamed("buttonLoadPrivateKey", "contract");
    wd.setText("lineEditServerCertificate", ca_cert_path_);
    wd.setText("lineEditClientCertificate", client_cert_path_);
    wd.setText("lineEditPrivateKey", private_key_path_);

    // OK gating: connected-only, like every discovery-capable streamer
    // (foxglove/pj_bridge/webrtc gate on connected_, ros2 on discovery).
    // Encoding availability still matters: no parsers -> nothing can decode.
    wd.setOkEnabled(connected_ && has_encodings);

    return wd.toJson();
  }

  bool onTick() override {
    // Check if new topics have arrived from the MQTT callback
    bool new_topics = false;
    {
      std::lock_guard<std::mutex> lock(topics_mutex_);
      if (topics_dirty_) {
        topics_dirty_ = false;
        new_topics = true;
      }
    }
    return new_topics;
  }

  bool onTextChanged(std::string_view widget_name, std::string_view text) override {
    if (widget_name == "lineEditHost") {
      broker_address_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditPort") {
      if (const auto port = PJ::sdk::parsePort(text)) {
        port_ = *port;
      }
      return false;
    }
    if (widget_name == "lineEditUsername") {
      username_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditPassword") {
      password_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditTopicFilter") {
      topic_filter_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditFilter") {
      view_filter_lower_ = PJ::sdk::lowerAscii(std::string(text));
      return true;  // re-render: the topic list narrows/expands live
    }
    // Certificate paths are editable: typing a path is equivalent to picking one.
    if (widget_name == "lineEditServerCertificate") {
      ca_cert_path_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditClientCertificate") {
      client_cert_path_ = std::string(text);
      return false;
    }
    if (widget_name == "lineEditPrivateKey") {
      private_key_path_ = std::string(text);
      return false;
    }
    return false;
  }

  bool onFileSelected(std::string_view widget_name, std::string_view path) override {
    if (widget_name == "buttonLoadServerCertificate") {
      ca_cert_path_ = std::string(path);
      return true;
    }
    if (widget_name == "buttonLoadClientCertificate") {
      client_cert_path_ = std::string(path);
      return true;
    }
    if (widget_name == "buttonLoadPrivateKey") {
      private_key_path_ = std::string(path);
      return true;
    }
    return false;
  }

  bool onIndexChanged(std::string_view widget_name, int index) override {
    if (widget_name == "comboBoxVersion") {
      protocol_version_index_ = index;
      return false;
    }
    if (widget_name == "comboBoxQoS") {
      qos_ = index;
      return false;
    }
    if (widget_name == "comboBoxProtocol") {
      encoding_ = PJ::sdk::encodingAt(index, available_encodings_);
      return false;
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "checkBoxSecurity") {
      use_ssl_ = checked;
      return false;
    }
    return false;
  }

  bool onSelectionChanged(std::string_view widget_name, const std::vector<std::string>& selected) override {
    if (widget_name == "listWidget") {
      // The host reports only the VISIBLE selection under the active view
      // filter. Preserve selections the filter currently hides; otherwise
      // selecting while a filter hides a previously selected topic would
      // silently drop the hidden one (same contract as the foxglove picker).
      {
        std::lock_guard<std::mutex> lock(topics_mutex_);
        selected_topics_ = PJ::sdk::mergeVisibleSelection(
            selected_topics_, selected,
            [this](const std::string& name) {
              // Not visible = filtered out OR not (yet) discovered — e.g. a
              // topic restored from saved config before connecting.
              return discovered_topics_.count(name) != 0 && matchesViewFilter(name);
            },
            [](const std::string&) { return true; });
      }
      return false;
    }
    return false;
  }

  bool onClicked(std::string_view widget_name) override {
    if (widget_name == "buttonConnect") {
      if (connected_) {
        disconnectBroker();
      } else {
        connectBroker();
      }
      return true;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {
    disconnectBroker();
  }
  void onRejected() override {
    disconnectBroker();
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["address"] = broker_address_;
    cfg["port"] = port_;
    cfg["username"] = username_;
    cfg["password"] = password_;
    cfg["protocol_version"] = protocol_version_index_;
    cfg["qos"] = qos_;
    cfg["topics"] = topic_filter_;
    cfg["selected_topics"] = selected_topics_;
    cfg["default_encoding"] = encoding_;
    cfg["use_ssl"] = use_ssl_;
    cfg["ca_cert_path"] = ca_cert_path_;
    cfg["client_cert_path"] = client_cert_path_;
    cfg["private_key_path"] = private_key_path_;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    broker_address_ = cfg.value("address", std::string("localhost"));
    port_ = cfg.value("port", 1883);
    username_ = cfg.value("username", std::string{});
    password_ = cfg.value("password", std::string{});
    protocol_version_index_ = cfg.value("protocol_version", 1);
    qos_ = cfg.value("qos", 0);
    topic_filter_ = cfg.value("topics", std::string("#"));
    encoding_ = cfg.value("default_encoding", std::string("json"));
    use_ssl_ = cfg.value("use_ssl", false);
    ca_cert_path_ = cfg.value("ca_cert_path", std::string{});
    client_cert_path_ = cfg.value("client_cert_path", std::string{});
    private_key_path_ = cfg.value("private_key_path", std::string{});
    if (cfg.contains("selected_topics") && cfg["selected_topics"].is_array()) {
      selected_topics_.clear();
      for (const auto& t : cfg["selected_topics"]) {
        if (t.is_string()) {
          selected_topics_.push_back(t.get<std::string>());
        }
      }
    }
    return true;
  }

 private:
  /// Case-insensitive substring match of the active view filter against the
  /// topic name. An empty filter matches everything. This is the band's
  /// client-side list filter — unrelated to topic_filter_, the broker-side
  /// MQTT subscription pattern. The filter is stored pre-lowercased, so only
  /// the topic pays a lowercase pass here.
  bool matchesViewFilter(const std::string& topic) const {
    return view_filter_lower_.empty() || PJ::sdk::lowerAscii(topic).find(view_filter_lower_) != std::string::npos;
  }

  void connectBroker() {
    const pj::mqtt_support::ConnectionSettings settings{
        .address = broker_address_,
        .port = port_,
        .username = username_,
        .password = password_,
        .protocol_version = protocol_version_index_,
        .use_ssl = use_ssl_,
        .ca_cert_path = ca_cert_path_,
        .client_cert_path = client_cert_path_,
        .private_key_path = private_key_path_,
    };

    try {
      discovery_client_ =
          std::make_unique<mqtt::async_client>(pj::mqtt_support::brokerUri(settings), "pj_mqtt_discovery");

      // Collect discovered topic names from incoming messages
      discovery_client_->set_message_callback([this](mqtt::const_message_ptr msg) {
        std::lock_guard<std::mutex> lock(topics_mutex_);
        if (discovered_topics_.insert(msg->get_topic()).second) {
          topics_dirty_ = true;
        }
      });

      discovery_client_->connect(pj::mqtt_support::makeConnectOptions(settings))->wait();
      // Subscribe to the user's topic filter to discover topics
      std::string sub_filter = topic_filter_.empty() ? "#" : topic_filter_;
      discovery_client_->subscribe(sub_filter, 0)->wait();
      connected_ = true;
      last_connect_error_.clear();
    } catch (const mqtt::exception& e) {
      last_connect_error_ = e.what();
      discovery_client_.reset();
      connected_ = false;
    }
  }

  void disconnectBroker() {
    if (discovery_client_) {
      try {
        if (discovery_client_->is_connected()) {
          discovery_client_->disconnect()->wait();
        }
      } catch (...) {}
      discovery_client_.reset();
    }
    connected_ = false;
    // Drop the discovered catalog: it belongs to the broker we just left, and
    // stale entries would show phantom topics (and count as "visible" for the
    // selection merge) after reconnecting to a different broker. The user's
    // selected_topics_ survive — they are restored/merged against whatever the
    // next connection actually delivers (same policy as the foxglove picker).
    {
      std::lock_guard<std::mutex> lock(topics_mutex_);
      discovered_topics_.clear();
    }
  }

  std::vector<std::string> available_encodings_;

  std::string broker_address_ = "localhost";
  int port_ = 1883;
  std::string username_;
  std::string password_;
  int protocol_version_index_ = 1;  // MQTT 3.1.1
  int qos_ = 0;
  std::string topic_filter_ = "#";
  std::string view_filter_lower_;
  std::string encoding_ = "json";
  bool use_ssl_ = false;
  std::string ca_cert_path_;
  std::string client_cert_path_;
  std::string private_key_path_;

  // Dialog-time discovery state
  bool connected_ = false;
  std::string last_connect_error_;
  std::unique_ptr<mqtt::async_client> discovery_client_;
  std::mutex topics_mutex_;
  std::set<std::string> discovered_topics_;
  std::vector<std::string> selected_topics_;
  bool topics_dirty_ = false;
};

}  // namespace
