#pragma once

#include <nlohmann/json.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/streaming_dialog.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

#include "datastream_zmq_ui.hpp"
#include "zmq_manifest.hpp"

namespace {

class ZmqDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  /// Called by the data source in loadConfig(), once the runtime host is bound.
  void setAvailableEncodings(std::vector<std::string> encodings) {
    available_encodings_ = std::move(encodings);
  }

  std::string manifest() const override {
    return kZmqManifest;
  }

  std::string ui_content() const override {
    return kDataStreamZmqUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    wd.setChecked("radioConnect", connect_mode_);
    wd.setChecked("radioBind", !connect_mode_);

    wd.setItems("comboBox", {"tcp://", "ipc://", "pgm://"});
    wd.setCurrentIndex("comboBox", transportToIndex(transport_));

    // IPC has no port.
    bool is_ipc = (transport_ == "ipc://");
    wd.setText("lineEditAddress", address_);
    wd.setText("lineEditPort", std::to_string(port_));
    wd.setEnabled("lineEditPort", !is_ipc);

    const bool has_encodings = PJ::sdk::writeEncodingSelector(wd, "comboBoxProtocol", available_encodings_, encoding_);

    wd.setText("lineEditTopics", topic_filter_);

    // No parsers, nothing can decode.
    wd.setOkEnabled(has_encodings);

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
      return true;  // re-render: the port field's enabled state changes
    }
    if (widget_name == "comboBoxProtocol") {
      encoding_ = PJ::sdk::encodingAt(index, available_encodings_);
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
      if (const auto port = PJ::sdk::parsePort(text)) {
        port_ = *port;
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
    if (cfg.is_discarded()) {
      return false;
    }
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
    if (t == "ipc://") {
      return 1;
    }
    if (t == "pgm://") {
      return 2;
    }
    return 0;  // tcp://
  }

  static std::string indexToTransport(int idx) {
    switch (idx) {
      case 1:
        return "ipc://";
      case 2:
        return "pgm://";
      default:
        return "tcp://";
    }
  }

  std::vector<std::string> available_encodings_;

  std::string address_ = "localhost";
  int port_ = 9872;
  std::string transport_ = "tcp://";
  bool connect_mode_ = true;
  std::string topic_filter_;
  std::string encoding_ = "json";
};

}  // namespace
