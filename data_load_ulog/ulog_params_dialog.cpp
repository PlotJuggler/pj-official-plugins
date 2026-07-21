#include "ulog_params_dialog.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <type_traits>
#include <ulog_cpp/data_container.hpp>
#include <ulog_cpp/reader.hpp>
#include <variant>
#include <vector>

// Generated at configure time
#include "ulog_manifest.hpp"
#include "ulog_params_ui.hpp"

namespace ulog_detail {

/// %g rendering + sort key for one scalar info value. The key is always a
/// DOUBLE, deliberately: the Info table's single Value column mixes integer-
/// and float-typed fields, and the host refuses a column whose keys mix the
/// two classes (no exact cross-class order exists) — native integer keys would
/// get the whole column's keys dropped and revert it to text sorting. A double
/// is exact for every realistic info scalar (uint32 counters, µs-epoch
/// timestamps ≪ 2^53), and it beats the %g display text, which rounds far
/// earlier. bool and char fold into one byte branch via unsigned char —
/// ulog_cpp's own as<> convention — so >=128 stays positive on signed-char
/// platforms.
template <typename T>
PJ::TableItem scalarInfoCell(T v) {
  char buf[64];
  const double key = std::is_same_v<T, char> || std::is_same_v<T, bool>
                         ? static_cast<double>(static_cast<unsigned char>(v))
                         : static_cast<double>(v);
  std::snprintf(buf, sizeof(buf), "%g", key);
  return PJ::TableItem(key, buf);
}

/// Convert a ulog_cpp::MessageInfo value to a table cell.
/// String values render as text and sort by it (no key). Numeric values keep
/// their %g rendering for display but carry the full-precision double as the
/// sort key — %g is lossy, so distinct values would otherwise collapse into
/// ties. Array-typed values show their FIRST element (matching ulog_cpp's
/// scalar accessor semantics); empty arrays and anything unconvertible render
/// as keyless "N/A".
PJ::TableItem infoValueToItem(const ulog_cpp::MessageInfo& info) {
  try {
    return std::visit(
        [](const auto& v) -> PJ::TableItem {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, std::string>) {
            return PJ::TableItem(v);
          } else if constexpr (std::is_arithmetic_v<T>) {
            return scalarInfoCell(v);
          } else {
            if (v.empty()) {
              return PJ::TableItem("N/A");
            }
            using Elem = typename T::value_type;
            return scalarInfoCell(static_cast<Elem>(v.front()));
          }
        },
        info.value().asNativeTypeVariant());
  } catch (...) {}
  return PJ::TableItem("N/A");
}

void ULogParamsDialog::setFilePath(const std::string& filepath) {
  filepath_ = filepath;
  parseFile();
}

std::string ULogParamsDialog::manifest() const {
  return kUlogManifest;
}

std::string ULogParamsDialog::ui_content() const {
  return kULogParamsUi;
}

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
  if (cfg.is_discarded()) {
    return false;
  }
  filepath_ = cfg.value("filepath", std::string{});
  if (!filepath_.empty()) {
    parseFile();
  }
  return true;
}

void ULogParamsDialog::parseFile() {
  info_rows_.clear();
  param_rows_.clear();
  log_rows_.clear();

  if (filepath_.empty()) {
    return;
  }

  std::ifstream file(filepath_, std::ios::binary);
  if (!file.is_open()) {
    return;
  }

  auto data_container = std::make_shared<ulog_cpp::DataContainer>(ulog_cpp::DataContainer::StorageConfig::FullLog);
  ulog_cpp::Reader reader{data_container};

  static constexpr size_t kChunkSize = 65536;
  std::vector<uint8_t> buffer(kChunkSize);
  while (file) {
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(kChunkSize));
    auto count = static_cast<size_t>(file.gcount());
    if (count == 0) {
      break;
    }
    reader.readChunk(buffer.data(), static_cast<int>(count));
  }

  // Info tab: messageInfo() — sys_*, ver_*, etc.
  for (const auto& [key, info] : data_container->messageInfo()) {
    info_rows_.push_back({key, infoValueToItem(info)});
  }

  // Properties tab: initial parameters
  for (const auto& [param_name, param] : data_container->initialParameters()) {
    PJ::TableItem value_item("N/A");
    try {
      const double value = param.value().as<double>();
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%g", value);
      value_item = PJ::TableItem(value, buf);
    } catch (...) {}
    param_rows_.push_back({param_name, value_item});
  }

  // Message Logs tab
  uint64_t start_us = data_container->fileHeader().header().timestamp;
  for (const auto& log : data_container->logging()) {
    auto ts_us = static_cast<uint64_t>(log.timestamp());
    double rel_s = (ts_us >= start_us) ? static_cast<double>(ts_us - start_us) / 1e6 : 0.0;
    char tbuf[32];
    std::snprintf(tbuf, sizeof(tbuf), "%.2f", rel_s);
    log_rows_.push_back({PJ::TableItem(rel_s, tbuf), std::string(log.logLevelStr()), std::string(log.message())});
  }
}

}  // namespace ulog_detail
