#pragma once

#include <algorithm>
#include <iterator>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

#include "datastream_udp_ui.hpp"
#include "udp_manifest.hpp"

namespace {

/// Dialog plugin for the UDP Server.
/// Configures address, port and message encoding.
class UdpDialog : public PJ::DialogPluginTyped {
 public:
  /// Set the available encodings from the runtime host.
  /// Called by the owning DataSource after runtime host is bound (in loadConfig).
  void setAvailableEncodings(std::vector<std::string> encodings) {
    available_encodings_ = std::move(encodings);
  }

  // --- Dialog protocol ---

  std::string manifest() const override {
    return kUdpManifest;
  }

  std::string ui_content() const override {
    return kDataStreamUdpUi;
  }

  std::string widget_data() override {
    PJ::WidgetData wd;

    wd.setText("lineEditAddress", address_);
    wd.setText("lineEditPort", std::to_string(port_));

    auto encodings = available_encodings_;
    bool has_encodings = !encodings.empty();
    if (has_encodings) {
      wd.setItems("comboBoxProtocol", encodings);
      wd.setCurrentIndex("comboBoxProtocol", encodingToIndex(encoding_, encodings));
    } else {
      wd.setItems("comboBoxProtocol", {"(no parsers available)"});
      wd.setCurrentIndex("comboBoxProtocol", 0);
      wd.setEnabled("comboBoxProtocol", false);
    }

    wd.setOkEnabled(has_encodings);

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
    return false;
  }

  bool onIndexChanged(std::string_view widget_name, int index) override {
    if (widget_name == "comboBoxProtocol") {
      encoding_ = indexToEncoding(index, available_encodings_);
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
    cfg["default_encoding"] = encoding_;
    return cfg.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return false;
    }
    address_ = cfg.value("address", std::string("127.0.0.1"));
    port_ = cfg.value("port", 9870);
    encoding_ = cfg.value("default_encoding", std::string("json"));
    return true;
  }

 private:
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

  std::vector<std::string> available_encodings_;

  std::string address_ = "127.0.0.1";
  int port_ = 9870;
  std::string encoding_ = "json";
};

}  // namespace
