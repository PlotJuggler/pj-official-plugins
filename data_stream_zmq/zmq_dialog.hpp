#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "datastream_zmq_ui.hpp"
#include "zmq_manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// Dialog plugin for the ZMQ Subscriber.
/// Uses the original .ui layout: connect/bind radios, transport combo,
/// address/port fields, protocol combo, topic filter.
class ZmqDialog : public PJ::DialogPluginTyped {
  using PJ::DialogPluginTyped::onValueChanged;

 public:
  /// Callback type for querying available encodings from the runtime host.
  /// Returns JSON array string, e.g. ["json","cbor","protobuf"], or empty if unavailable.
  using EncodingsCallback = std::function<std::string_view()>;

  /// Set the callback to query available encodings from the runtime host.
  /// Called by the owning DataSource after runtime host is bound.
  void setEncodingsCallback(EncodingsCallback callback) { encodings_callback_ = std::move(callback); }

  // --- Dialog protocol ---

  std::string manifest() const override { return kZmqManifest; }

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

    // Protocol combo — dynamically populated from available parsers
    auto encodings = getAvailableEncodings();
    bool has_encodings = !encodings.empty();
    if (has_encodings) {
      wd.setItems("comboBoxProtocol", encodings);
      wd.setCurrentIndex("comboBoxProtocol", encodingToIndex(encoding_, encodings));
    } else {
      wd.setItems("comboBoxProtocol", {"(no parsers available)"});
      wd.setCurrentIndex("comboBoxProtocol", 0);
      wd.setEnabled("comboBoxProtocol", false);
    }

    // Topic filter
    wd.setText("lineEditTopics", topic_filter_);

    // Disable OK if no encodings available
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
      return false;
    }
    if (widget_name == "comboBoxProtocol") {
      auto encodings = getAvailableEncodings();
      encoding_ = indexToEncoding(index, encodings);
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

/// Query available encodings from runtime host.
  /// Returns empty vector if no parsers are loaded or host doesn't support the method.
  std::vector<std::string> getAvailableEncodings() const {
    if (encodings_callback_) {
      auto json_str = encodings_callback_();
      if (!json_str.empty()) {
        auto arr = nlohmann::json::parse(json_str, nullptr, false);
        if (arr.is_array()) {
          std::vector<std::string> result;
          result.reserve(arr.size());
          for (const auto& e : arr) {
            if (e.is_string()) {
              result.push_back(e.get<std::string>());
            }
          }
          return result;
        }
      }
    }
    return {};
  }

  static int encodingToIndex(const std::string& e, const std::vector<std::string>& encodings) {
    auto it = std::find(encodings.begin(), encodings.end(), e);
    return (it != encodings.end()) ? static_cast<int>(std::distance(encodings.begin(), it)) : 0;
  }

  static std::string indexToEncoding(int idx, const std::vector<std::string>& encodings) {
    if (idx >= 0 && idx < static_cast<int>(encodings.size())) {
      return encodings[static_cast<size_t>(idx)];
    }
    return encodings.empty() ? "json" : encodings[0];
  }

  EncodingsCallback encodings_callback_;

  std::string address_ = "localhost";
  int port_ = 9872;
  std::string transport_ = "tcp://";
  bool connect_mode_ = true;
  std::string topic_filter_;
  std::string encoding_ = "json";
};

}  // namespace
