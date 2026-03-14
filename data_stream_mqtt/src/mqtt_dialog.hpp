#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "datastream_mqtt_ui.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

/// Dialog plugin for the MQTT Subscriber.
/// Uses the original .ui layout with Connection + Security tabs.
/// Widget names match the original DataStreamMQTT .ui file.
class MqttDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  // --- Dialog protocol ---

  std::string manifest() const override {
    return R"({"name":"MQTT Subscriber","version":"1.0.0"})";
  }

  std::string ui_content() const override { return kDataStreamMqttUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Connection tab widgets (original names from .ui)
    wd.setText("lineEditHost", broker_address_);
    wd.setText("lineEditPort", std::to_string(port_));
    wd.setText("lineEditUsername", username_);
    wd.setText("lineEditPassword", password_);

    // Protocol version combo
    wd.setCurrentIndex("comboBoxVersion", protocol_version_index_);

    // QoS combo
    wd.setCurrentIndex("comboBoxQoS", qos_);

    // Topic filter
    wd.setText("lineEditTopics", topic_filter_);

    // Protocol combo
    wd.setItems("comboBoxProtocol", {"json", "protobuf", "cdr"});
    wd.setCurrentIndex("comboBoxProtocol", encodingToIndex(encoding_));

    wd.setOkEnabled(true);

    return wd.toJson();
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
    if (widget_name == "lineEditTopics") {
      topic_filter_ = std::string(text);
      return false;
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
      encoding_ = indexToEncoding(index);
      return false;
    }
    return false;
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "checkBoxSSL") {
      use_ssl_ = checked;
      return false;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["address"] = broker_address_;
    cfg["port"] = port_;
    cfg["username"] = username_;
    cfg["password"] = password_;
    cfg["protocol_version"] = protocol_version_index_;
    cfg["qos"] = qos_;
    cfg["topics"] = topic_filter_;
    cfg["default_encoding"] = encoding_;
    cfg["use_ssl"] = use_ssl_;
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
    return true;
  }

 private:
  static int encodingToIndex(const std::string& e) {
    if (e == "protobuf") return 1;
    if (e == "cdr") return 2;
    return 0;  // json
  }

  static std::string indexToEncoding(int idx) {
    switch (idx) {
      case 1: return "protobuf";
      case 2: return "cdr";
      default: return "json";
    }
  }

  std::string broker_address_ = "localhost";
  int port_ = 1883;
  std::string username_;
  std::string password_;
  int protocol_version_index_ = 1;  // MQTT 3.1.1
  int qos_ = 0;
  std::string topic_filter_ = "#";
  std::string encoding_ = "json";
  bool use_ssl_ = false;
};

}  // namespace
