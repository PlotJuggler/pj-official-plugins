#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "datastream_zmq_ui.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

/// Dialog plugin for the ZMQ Subscriber.
/// Uses the original .ui layout: connect/bind radios, transport combo,
/// address/port fields, protocol combo, topic filter.
class ZmqDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  // --- Dialog protocol ---

  std::string manifest() const override {
    return R"({"name":"ZMQ Subscriber","version":"1.0.0"})";
  }

  std::string ui_content() const override { return kDataStreamZmqUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;

    // Connection mode
    wd.setChecked("radioConnect", connect_mode_);
    wd.setChecked("radioBind", !connect_mode_);

    // Transport combo (matches original .ui: comboBox with tcp://, ipc://, pgm://)
    wd.setItems("comboBox", {"tcp://", "ipc://", "pgm://"});
    wd.setCurrentIndex("comboBox", transportToIndex(transport_));

    // Address and port
    wd.setText("lineEditAddress", address_);
    wd.setText("lineEditPort", std::to_string(port_));

    // Protocol combo
    wd.setItems("comboBoxProtocol", {"json", "protobuf", "cdr"});
    wd.setCurrentIndex("comboBoxProtocol", encodingToIndex(encoding_));

    // Topic filter
    wd.setText("lineEditTopics", topic_filter_);

    wd.setOkEnabled(true);

    return wd.toJson();
  }

  bool onToggled(std::string_view widget_name, bool checked) override {
    if (widget_name == "radioConnect") {
      connect_mode_ = checked;
      return false;
    }
    if (widget_name == "radioBind") {
      connect_mode_ = !checked;
      return false;
    }
    return false;
  }

  bool onIndexChanged(std::string_view widget_name, int index) override {
    if (widget_name == "comboBox") {
      transport_ = indexToTransport(index);
      return false;
    }
    if (widget_name == "comboBoxProtocol") {
      encoding_ = indexToEncoding(index);
      return false;
    }
    return false;
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
    if (widget_name == "lineEditTopics") {
      topic_filter_ = std::string(text);
      return false;
    }
    return false;
  }

  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["address"] = address_;
    cfg["port"] = port_;
    cfg["transport"] = transport_;
    cfg["mode"] = connect_mode_ ? "connect" : "bind";
    cfg["topics"] = topic_filter_;
    cfg["default_encoding"] = encoding_;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) return false;
    address_ = cfg.value("address", std::string("localhost"));
    port_ = cfg.value("port", 9872);
    transport_ = cfg.value("transport", std::string("tcp://"));
    connect_mode_ = cfg.value("mode", std::string("connect")) == "connect";
    topic_filter_ = cfg.value("topics", std::string{});
    encoding_ = cfg.value("default_encoding", std::string("json"));
    return true;
  }

 private:
  static int transportToIndex(const std::string& t) {
    if (t == "ipc://") return 1;
    if (t == "pgm://") return 2;
    return 0;  // tcp://
  }

  static std::string indexToTransport(int idx) {
    switch (idx) {
      case 1: return "ipc://";
      case 2: return "pgm://";
      default: return "tcp://";
    }
  }

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

  std::string address_ = "localhost";
  int port_ = 9872;
  std::string transport_ = "tcp://";
  bool connect_mode_ = true;
  std::string topic_filter_;
  std::string encoding_ = "json";
};

}  // namespace
