#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "datastream_mqtt_ui.hpp"
#include "mqtt_manifest.hpp"

#include <nlohmann/json.hpp>

#include <mqtt/async_client.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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

  std::string manifest() const override { return kMqttManifest; }

  std::string ui_content() const override { return kDataStreamMqttUi; }

  ~MqttDialog() override { disconnectBroker(); }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Connection tab widgets
    wd.setText("lineEditHost", broker_address_);
    wd.setText("lineEditPort", std::to_string(port_));
    wd.setText("lineEditUsername", username_);
    wd.setText("lineEditPassword", password_);
    wd.setEnabled("lineEditHost", !connected_);
    wd.setEnabled("lineEditPort", !connected_);

    // Connect button state + error feedback
    wd.setButtonText("buttonConnect", connected_ ? "Disconnect" : "Connect");
    wd.setText("label_12",
               last_connect_error_.empty()
                   ? (connected_ ? "Connected — select topics below" : "Select a specific topic:")
                   : ("Connection error: " + last_connect_error_));

    // Protocol version combo
    wd.setCurrentIndex("comboBoxVersion", protocol_version_index_);

    // QoS combo
    wd.setCurrentIndex("comboBoxQoS", qos_);

    // SSL checkbox
    wd.setChecked("checkBoxSecurity", use_ssl_);

    // Topic filter
    wd.setText("lineEditTopicFilter", topic_filter_);

    // Discovered topic list (from live MQTT subscription)
    {
      std::lock_guard<std::mutex> lock(topics_mutex_);
      std::vector<std::string> topic_list(discovered_topics_.begin(), discovered_topics_.end());
      wd.setListItems("listWidget", topic_list);
      wd.setSelectedItems("listWidget", selected_topics_);
    }

    // Protocol combo — dynamically populated from available parsers
    bool has_encodings = !available_encodings_.empty();
    if (has_encodings) {
      wd.setItems("comboBoxProtocol", available_encodings_);
      wd.setCurrentIndex("comboBoxProtocol", encodingToIndex(encoding_));
    } else {
      wd.setItems("comboBoxProtocol", {"(no parsers available)"});
      wd.setCurrentIndex("comboBoxProtocol", 0);
      wd.setEnabled("comboBoxProtocol", false);
    }

    // TLS certificate file pickers
    wd.setFilePicker("buttonLoadServerCertificate", "...", "*.pem *.crt *.cer", "Select Server CA Certificate");
    wd.setFilePicker("buttonLoadClientCertificate", "...", "*.pem *.crt *.cer", "Select Client Certificate");
    wd.setFilePicker("buttonLoadPrivateKey", "...", "*.pem *.key", "Select Private Key");
    wd.setText("labelServerCertificate", ca_cert_path_.empty() ? "(none)" : filenameFromPath(ca_cert_path_));
    wd.setText("labelClientCertificate", client_cert_path_.empty() ? "(none)" : filenameFromPath(client_cert_path_));
    wd.setText("labelPrivateKey", private_key_path_.empty() ? "(none)" : filenameFromPath(private_key_path_));
    wd.setEnabled("buttonEraseServerCertificate", !ca_cert_path_.empty());
    wd.setEnabled("buttonEraseClientCertificate", !client_cert_path_.empty());
    wd.setEnabled("buttonErasePrivateKey", !private_key_path_.empty());

    // Disable OK if no encodings available
    wd.setOkEnabled(has_encodings);

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
      auto val = std::atoi(std::string(text).c_str());
      if (val > 0 && val <= 65535) {
        port_ = val;
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
    return false;
  }

  bool onFileSelected(std::string_view widget_name, std::string_view path) override {
    if (widget_name == "buttonLoadServerCertificate") { ca_cert_path_ = std::string(path); return true; }
    if (widget_name == "buttonLoadClientCertificate") { client_cert_path_ = std::string(path); return true; }
    if (widget_name == "buttonLoadPrivateKey") { private_key_path_ = std::string(path); return true; }
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
      encoding_ = indexToEncoding(index);
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

  bool onSelectionChanged(std::string_view widget_name,
                          const std::vector<std::string>& selected) override {
    if (widget_name == "listWidget") {
      selected_topics_ = selected;
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
    if (widget_name == "buttonEraseServerCertificate") { ca_cert_path_.clear(); return true; }
    if (widget_name == "buttonEraseClientCertificate") { client_cert_path_.clear(); return true; }
    if (widget_name == "buttonErasePrivateKey") { private_key_path_.clear(); return true; }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override { disconnectBroker(); }
  void onRejected() override { disconnectBroker(); }

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
    if (cfg.is_discarded()) return false;
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
        if (t.is_string()) selected_topics_.push_back(t.get<std::string>());
      }
    }
    return true;
  }

 private:
  int encodingToIndex(const std::string& e) const {
    auto it = std::find(available_encodings_.begin(), available_encodings_.end(), e);
    return (it != available_encodings_.end()) ? static_cast<int>(std::distance(available_encodings_.begin(), it)) : 0;
  }

  std::string indexToEncoding(int idx) const {
    if (idx >= 0 && idx < static_cast<int>(available_encodings_.size())) {
      return available_encodings_[static_cast<size_t>(idx)];
    }
    return available_encodings_.empty() ? "json" : available_encodings_[0];
  }

  static std::string filenameFromPath(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return (pos != std::string::npos) ? path.substr(pos + 1) : path;
  }

  void connectBroker() {
    std::string scheme = use_ssl_ ? "ssl://" : "tcp://";
    std::string uri = scheme + broker_address_ + ":" + std::to_string(port_);

    try {
      discovery_client_ = std::make_unique<mqtt::async_client>(uri, "pj_mqtt_discovery");

      // Collect discovered topic names from incoming messages
      discovery_client_->set_message_callback([this](mqtt::const_message_ptr msg) {
        std::lock_guard<std::mutex> lock(topics_mutex_);
        if (discovered_topics_.insert(msg->get_topic()).second) {
          topics_dirty_ = true;
        }
      });

      mqtt::connect_options opts;
      opts.set_clean_session(true);
      opts.set_connect_timeout(std::chrono::seconds(5));
      if (protocol_version_index_ == 0) {
        opts.set_mqtt_version(MQTTVERSION_3_1);
      } else if (protocol_version_index_ == 2) {
        opts.set_mqtt_version(MQTTVERSION_5);
      } else {
        opts.set_mqtt_version(MQTTVERSION_3_1_1);
      }
      if (!username_.empty()) {
        opts.set_user_name(username_);
        opts.set_password(password_);
      }
      if (use_ssl_) {
        mqtt::ssl_options ssl_opts;
        if (!ca_cert_path_.empty()) ssl_opts.set_trust_store(ca_cert_path_);
        if (!client_cert_path_.empty()) ssl_opts.set_key_store(client_cert_path_);
        if (!private_key_path_.empty()) ssl_opts.set_private_key(private_key_path_);
        opts.set_ssl(ssl_opts);
      }

      discovery_client_->connect(opts)->wait();
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
      } catch (...) {
      }
      discovery_client_.reset();
    }
    connected_ = false;
  }

  std::vector<std::string> available_encodings_;

  std::string broker_address_ = "localhost";
  int port_ = 1883;
  std::string username_;
  std::string password_;
  int protocol_version_index_ = 1;  // MQTT 3.1.1
  int qos_ = 0;
  std::string topic_filter_ = "#";
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
