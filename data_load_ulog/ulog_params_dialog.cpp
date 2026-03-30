#include "ulog_params_dialog.hpp"

#include <pj_plugins/sdk/widget_data.hpp>

#include <ulog_cpp/data_container.hpp>
#include <ulog_cpp/reader.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Generated from ulog_params.ui at configure time
#include "ulog_params_ui.hpp"

namespace ulog_detail {

/// Convert a ulog_cpp::MessageInfo value to a display string.
/// Tries string first (for char-array info fields), then falls back to double.
std::string infoValueToString(const ulog_cpp::MessageInfo& info) {
  try {
    return info.value().as<std::string>();
  } catch (...) {
  }
  try {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", info.value().as<double>());
    return buf;
  } catch (...) {
  }
  return "N/A";
}

void ULogParamsDialog::setFilePath(const std::string& filepath) {
  filepath_ = filepath;
  parseFile();
}

std::string ULogParamsDialog::manifest() const {
  return R"({"name":"ULog Parameters","version":"1.0.0"})";
}

std::string ULogParamsDialog::ui_content() const { return kULogParamsUi; }

std::string ULogParamsDialog::widget_data() {
  PJ::WidgetData wd;

  // Info tab — ULog info messages (sys_*, ver_*, ...)
  wd.setTableHeaders("tableInfo", {"Property", "Value"});
  wd.setTableRows("tableInfo", info_rows_);

  // Properties tab — initial parameters
  wd.setTableHeaders("tableParams", {"Property", "Value"});
  wd.setTableRows("tableParams", param_rows_);

  // Message Logs tab
  wd.setTableHeaders("tableMessageLogs", {"Timestamp", "Level", "Message"});
  wd.setTableRows("tableMessageLogs", log_rows_);

  return wd.toJson();
}

std::string ULogParamsDialog::saveConfig() const {
  return nlohmann::json{{"filepath", filepath_}}.dump();
}

bool ULogParamsDialog::loadConfig(std::string_view config_json) {
  auto cfg = nlohmann::json::parse(config_json, nullptr, false);
  if (cfg.is_discarded()) return false;
  filepath_ = cfg.value("filepath", std::string{});
  if (!filepath_.empty()) parseFile();
  return true;
}

void ULogParamsDialog::parseFile() {
  info_rows_.clear();
  param_rows_.clear();
  log_rows_.clear();

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

  // Info tab: messageInfo() — sys_*, ver_*, etc.
  for (const auto& [key, info] : data_container->messageInfo()) {
    info_rows_.push_back({key, infoValueToString(info)});
  }

  // Properties tab: initial parameters
  for (const auto& [param_name, param] : data_container->initialParameters()) {
    std::string value_str;
    try {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%g", param.value().as<double>());
      value_str = buf;
    } catch (...) {
      value_str = "N/A";
    }
    param_rows_.push_back({param_name, value_str});
  }

  // Message Logs tab
  uint64_t start_us = data_container->fileHeader().header().timestamp;
  for (const auto& log : data_container->logging()) {
    auto ts_us = static_cast<uint64_t>(log.timestamp());
    double rel_s = (ts_us >= start_us) ? static_cast<double>(ts_us - start_us) / 1e6 : 0.0;
    char tbuf[32];
    std::snprintf(tbuf, sizeof(tbuf), "%.2f", rel_s);
    log_rows_.push_back({std::string(tbuf), std::string(log.logLevelStr()), std::string(log.message())});
  }
}

}  // namespace ulog_detail
