#pragma once

#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include <ulog_cpp/data_container.hpp>
#include <ulog_cpp/reader.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// Generated from ulog_params.ui at configure time
#include "ulog_params_ui.hpp"

namespace {

class ULogParamsDialog : public PJ::DialogPluginTyped {
 public:
  void setFilePath(const std::string& filepath) {
    filepath_ = filepath;
    parseParameters();
  }

  // --- Dialog protocol ---

  std::string manifest() const override {
    return R"({"name":"ULog Parameters","version":"1.0.0"})";
  }

  std::string ui_content() const override { return kULogParamsUi; }

  std::string widget_data() override {
    PJ::WidgetData wd;

    std::string label = filepath_.empty() ? "No file selected" : "File: " + filepath_;
    wd.setLabel("labelFile", label);

    wd.setTableHeaders("tableParams", {"Parameter", "Value"});
    wd.setTableRows("tableParams", rows_);

    return wd.toJson();
  }

  std::string saveConfig() const override {
    return nlohmann::json{{"filepath", filepath_}}.dump();
  }

  bool loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) return false;
    filepath_ = cfg.value("filepath", std::string{});
    if (!filepath_.empty()) parseParameters();
    return true;
  }

  void onAccepted(std::string_view /*json*/) override {}
  void onRejected() override {}

 private:
  void parseParameters() {
    rows_.clear();

    if (filepath_.empty()) return;

    std::ifstream file(filepath_, std::ios::binary);
    if (!file.is_open()) return;

    auto data_container =
        std::make_shared<ulog_cpp::DataContainer>(ulog_cpp::DataContainer::StorageConfig::FullLog);
    ulog_cpp::Reader reader{data_container};

    static constexpr size_t kChunkSize = 65536;
    std::vector<uint8_t> buffer(kChunkSize);
    while (file) {
      file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(kChunkSize));
      auto count = static_cast<size_t>(file.gcount());
      if (count == 0) break;
      reader.readChunk(buffer.data(), static_cast<int>(count));
    }

    for (const auto& [param_name, param] : data_container->initialParameters()) {
      std::string value_str;
      try {
        double v = param.value().as<double>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", v);
        value_str = buf;
      } catch (...) {
        value_str = "N/A";
      }
      rows_.push_back({param_name, value_str});
    }
  }

  std::string filepath_;
  std::vector<std::vector<std::string>> rows_;
};

}  // namespace
