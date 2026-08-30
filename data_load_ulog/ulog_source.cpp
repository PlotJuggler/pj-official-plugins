#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>
#include <string_view>
#include <ulog_cpp/reader.hpp>
#include <vector>

#include "ulog_container.hpp"
#include "ulog_flatten.hpp"
#include "ulog_manifest.hpp"
#include "ulog_params_dialog.hpp"

namespace {

/// Map ULog BasicType to PJ::PrimitiveType.
PJ::PrimitiveType ulogTypeToPrimitive(ulog_cpp::Field::BasicType type) {
  switch (type) {
    case ulog_cpp::Field::BasicType::INT8:
      return PJ::PrimitiveType::kInt8;
    case ulog_cpp::Field::BasicType::UINT8:
      return PJ::PrimitiveType::kUint8;
    case ulog_cpp::Field::BasicType::INT16:
      return PJ::PrimitiveType::kInt16;
    case ulog_cpp::Field::BasicType::UINT16:
      return PJ::PrimitiveType::kUint16;
    case ulog_cpp::Field::BasicType::INT32:
      return PJ::PrimitiveType::kInt32;
    case ulog_cpp::Field::BasicType::UINT32:
      return PJ::PrimitiveType::kUint32;
    case ulog_cpp::Field::BasicType::INT64:
      return PJ::PrimitiveType::kInt64;
    case ulog_cpp::Field::BasicType::UINT64:
      return PJ::PrimitiveType::kUint64;
    case ulog_cpp::Field::BasicType::FLOAT:
      return PJ::PrimitiveType::kFloat32;
    case ulog_cpp::Field::BasicType::DOUBLE:
      return PJ::PrimitiveType::kFloat64;
    case ulog_cpp::Field::BasicType::BOOL:
      return PJ::PrimitiveType::kBool;
    case ulog_cpp::Field::BasicType::CHAR:
      return PJ::PrimitiveType::kUint8;
    default:
      return PJ::PrimitiveType::kFloat64;
  }
}

/// Read a single primitive value from raw ULog data at a given offset.
PJ::sdk::ValueRef readPrimitiveValue(const uint8_t* data, size_t offset, ulog_cpp::Field::BasicType type) {
  const uint8_t* p = data + offset;
  switch (type) {
    case ulog_cpp::Field::BasicType::INT8:
      return *reinterpret_cast<const int8_t*>(p);
    case ulog_cpp::Field::BasicType::UINT8:
      return *p;
    case ulog_cpp::Field::BasicType::INT16: {
      int16_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::UINT16: {
      uint16_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::INT32: {
      int32_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::UINT32: {
      uint32_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::INT64: {
      int64_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::UINT64: {
      uint64_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::FLOAT: {
      float v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::DOUBLE: {
      double v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::BOOL:
      return static_cast<bool>(*p);
    case ulog_cpp::Field::BasicType::CHAR:
      return static_cast<uint8_t>(*p);
    default:
      return PJ::NullValue{};
  }
}

/// Extract leaf values from a raw ULog record into `values`, in the same order
/// as collectFlatFieldNames. `char[N]` leaves become string_views into the
/// record (the record outlives the appendRecord call that consumes them).
/// Reads that would fall outside `raw_size` (a truncated or corrupt record)
/// push a NullValue instead, preserving alignment with the field-name list
/// rather than reading out of bounds.
void extractFlatValues(
    const uint8_t* raw_data, size_t raw_size, const ulog_cpp::MessageFormat& format,
    std::vector<PJ::sdk::ValueRef>& values) {
  ulog_flatten::forEachFlatLeaf(format, 0, [&](const ulog_flatten::FlatLeaf& leaf) {
    if (leaf.offset + leaf.size > raw_size) {
      values.push_back(PJ::NullValue{});
    } else if (leaf.is_string) {
      values.push_back(ulog_flatten::stringLeafView(raw_data, leaf.offset, leaf.size));
    } else {
      values.push_back(readPrimitiveValue(raw_data, leaf.offset, leaf.type));
    }
  });
}

class ULogSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  std::string saveConfig() const override {
    return nlohmann::json{{"filepath", filepath_}}.dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    filepath_ = cfg.value("filepath", std::string{});
    if (!filepath_.empty()) {
      dialog_.setFilePath(filepath_);
    }
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    if (filepath_.empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }

    std::ifstream file(filepath_, std::ios::binary);
    if (!file.is_open()) {
      return PJ::unexpected(std::string("cannot open file: ") + filepath_);
    }

    // Get file size for progress.
    file.seekg(0, std::ios::end);
    auto file_size = static_cast<uint64_t>(file.tellg());
    file.seekg(0);

    (void)runtimeHost().progressStart("Importing ULog", file_size, true);

    // Parse via ulog_cpp (ULogContainer adds the data-message clock for parameter changes).
    auto data_container = std::make_shared<ulog_container::ULogContainer>();
    ulog_cpp::Reader reader{data_container};

    static constexpr size_t kChunkSize = 65536;
    std::vector<uint8_t> buffer(kChunkSize);
    uint64_t bytes_read = 0;

    while (file) {
      file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(kChunkSize));
      auto count = static_cast<size_t>(file.gcount());
      if (count == 0) {
        break;
      }

      reader.readChunk(buffer.data(), static_cast<int>(count));
      bytes_read += count;

      if (runtimeHost().isStopRequested()) {
        // Stop reading but KEEP what's parsed: break out and fall through to the write
        // path so the messages decoded so far are committed. The host's "Stop and Keep"
        // then flushes them (Discard evicts the dataset host-side). Returning here would
        // leave the dataset empty — the bug this fixes.
        break;
      }
      (void)runtimeHost().progressUpdate(bytes_read);
    }

    if (data_container->hadFatalError()) {
      std::string err_msg = "ULog parse error";
      const auto& errors = data_container->parsingErrors();
      if (!errors.empty()) {
        err_msg += ": " + errors.front();
      }
      return PJ::unexpected(err_msg);
    }

    // Recoverable problems (mid-file corruption, unsupported appended-data
    // offsets, ...) don't abort the import: ulog_cpp resynchronises and keeps
    // going. Surface them so a partial result is never mistaken for a complete one.
    if (!data_container->parsingErrors().empty()) {
      const auto& errors = data_container->parsingErrors();
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "ULog: " + std::to_string(errors.size()) +
              " parsing problem(s), data may be incomplete. First: " + errors.front());
    }

    // Get file start timestamp (microseconds).
    uint64_t file_start_time_us = data_container->fileHeader().header().timestamp;

    // Iterate subscriptions grouped by name and multi_id.
    const auto& subs_map = data_container->subscriptionsByNameAndMultiId();

    // Detect which names have multiple IDs for suffix generation.
    std::map<std::string, int> name_max_multi_id;
    for (const auto& [key, sub] : subs_map) {
      auto it = name_max_multi_id.find(key.name);
      if (it == name_max_multi_id.end()) {
        name_max_multi_id[key.name] = key.multi_id;
      } else {
        it->second = std::max(it->second, key.multi_id);
      }
    }

    size_t total_series_count = 0;

    // Diagnostics are aggregated and reported once: a malformed log can define
    // thousands of subscriptions, and one message per problem would flood the
    // host (whose dedup is by exact text).
    std::vector<std::string> topics_without_timestamp;
    size_t short_records = 0;
    std::string first_short_record_topic;

    for (const auto& [key, sub] : subs_map) {
      if (!sub || sub->size() == 0) {
        continue;
      }

      // Build topic name, adding multi_id suffix when needed.
      std::string topic_name = key.name;
      auto max_it = name_max_multi_id.find(key.name);
      if (max_it != name_max_multi_id.end() && max_it->second > 0) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), ".%02d", key.multi_id);
        topic_name += buf;
      }

      // Collect flattened field names.
      std::vector<std::string> field_names;
      ulog_flatten::collectFlatFieldNames(*sub->format(), {}, field_names);
      if (field_names.empty()) {
        continue;
      }

      // Ensure topic and pre-register all fields.
      auto topic = writeHost().ensureTopic(topic_name);
      if (!topic) {
        return PJ::unexpected(topic.error());
      }

      std::vector<PJ::PrimitiveType> field_types;
      ulog_flatten::forEachFlatLeaf(*sub->format(), 0, [&](const ulog_flatten::FlatLeaf& leaf) {
        field_types.push_back(leaf.is_string ? PJ::PrimitiveType::kString : ulogTypeToPrimitive(leaf.type));
      });

      for (size_t fi = 0; fi < field_names.size() && fi < field_types.size(); ++fi) {
        auto field = writeHost().ensureField(*topic, field_names[fi], field_types[fi]);
        if (!field) {
          return PJ::unexpected(field.error());
        }
      }

      // The spec mandates a `uint64_t timestamp` field but not its position, so
      // locate it by name. A format without one (seen in the wild — original
      // plugin issue #1154) falls back to the sample index, exactly as the
      // original DataLoadULog did (`timestamps[i].value_or(i)` microseconds).
      const auto ts_offset = ulog_flatten::findTimestampOffset(*sub->format());
      if (!ts_offset) {
        topics_without_timestamp.push_back(topic_name);
      }
      const auto format_size = static_cast<size_t>(sub->format()->sizeBytes());

      // Write data records.
      std::vector<PJ::sdk::NamedFieldValue> row_fields;
      row_fields.reserve(field_names.size());
      std::vector<PJ::sdk::ValueRef> values;
      values.reserve(field_names.size());

      const auto& raw_samples = sub->rawSamples();
      for (size_t i = 0; i < raw_samples.size(); ++i) {
        const auto& raw = raw_samples[i].data();

        // ulog_cpp validates the message FRAME, not the payload against the
        // format. A record shorter than its format is corrupt: skip it rather
        // than fabricate a timestamp and null fields for it.
        if (raw.size() < format_size) {
          if (short_records++ == 0) {
            first_short_record_topic = topic_name;
          }
          continue;
        }

        // Extract the timestamp (uint64_t microseconds) and convert to nanoseconds.
        uint64_t timestamp_us = static_cast<uint64_t>(i);
        if (ts_offset) {
          std::memcpy(&timestamp_us, raw.data() + *ts_offset, sizeof(timestamp_us));
        }
        auto ts_ns = static_cast<int64_t>(timestamp_us) * 1000;

        // Extract all field values from raw bytes.
        values.clear();
        extractFlatValues(raw.data(), raw.size(), *sub->format(), values);

        // Build row.
        row_fields.clear();
        size_t count = std::min(values.size(), field_names.size());
        for (size_t j = 0; j < count; ++j) {
          row_fields.push_back({.name = field_names[j], .value = values[j]});
        }

        auto status = writeHost().appendRecord(
            *topic, PJ::Timestamp{ts_ns},
            PJ::Span<const PJ::sdk::NamedFieldValue>(row_fields.data(), row_fields.size()));
        if (!status) {
          return status;
        }
      }

      total_series_count += field_names.size();
    }

    // Joins up to `max_shown` names, then "... (+N more)".
    auto sample_list = [](const std::vector<std::string>& names, size_t max_shown) {
      std::string out;
      for (size_t i = 0; i < names.size() && i < max_shown; ++i) {
        out += (i ? ", " : "") + names[i];
      }
      if (names.size() > max_shown) {
        out += ", ... (+" + std::to_string(names.size() - max_shown) + " more)";
      }
      return out;
    };

    if (!topics_without_timestamp.empty()) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "ULog: " + std::to_string(topics_without_timestamp.size()) +
              " message format(s) have no timestamp field; the sample index is used as time: " +
              sample_list(topics_without_timestamp, 5));
    }
    if (short_records > 0) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "ULog: skipped " + std::to_string(short_records) +
                                                    " data record(s) shorter than their message format (first in '" +
                                                    first_short_record_topic + "')");
    }

    // Write parameters as `_parameters/<name>` timeseries: the initial snapshot
    // at file start, then every in-flight change at the data-message clock it
    // appeared under (PlotJuggler#1245). PX4 parameters are int32/float only;
    // anything else is collected and reported once instead of silently skipped.
    std::vector<std::string> non_numeric_params;
    auto write_parameter = [&](const std::string& param_name, const ulog_cpp::Parameter& param,
                               uint64_t ts_us) -> PJ::Status {
      double param_value = 0.0;
      try {
        param_value = param.value().as<double>();
      } catch (const std::exception&) {
        std::string str_val;
        try {
          str_val = param.value().as<std::string>();
        } catch (...) {}
        non_numeric_params.push_back(param_name + "=" + str_val);
        return PJ::okStatus();  // convert BEFORE ensureTopic: no empty topics
      }

      auto topic = writeHost().ensureTopic("_parameters/" + param_name);
      if (!topic) {
        return PJ::unexpected(topic.error());
      }
      auto ts_ns = static_cast<int64_t>(ts_us) * 1000;
      return writeHost().appendRecord(*topic, PJ::Timestamp{ts_ns}, {{.name = "value", .value = param_value}});
    };

    for (const auto& [param_name, param] : data_container->initialParameters()) {
      auto status = write_parameter(param_name, param, file_start_time_us);
      if (!status) {
        return status;
      }
    }
    for (const auto& change : data_container->timedChangedParameters()) {
      auto status = write_parameter(change.parameter.field().name(), change.parameter, change.timestamp_us);
      if (!status) {
        return status;
      }
    }
    if (!non_numeric_params.empty()) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kInfo,
          "ULog: " + std::to_string(non_numeric_params.size()) +
              " non-numeric parameter message(s) not plotted: " + sample_list(non_numeric_params, 5));
    }

    // Write file info metadata as `_info/<key>` topics (numeric values) and
    // report string values as info messages. Both ordinary INFO entries
    // (sys_name, ver_hw, ...) and the first instance of each INFO_MULTIPLE key.
    {
      auto write_info = [&](const std::string& key, const ulog_cpp::MessageInfo& info) {
        double val = 0.0;
        try {
          val = info.value().as<double>();
        } catch (...) {
          // Non-numeric info: try as string via reportMessage
          try {
            std::string str_val = info.value().as<std::string>();
            runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo, key + ": " + str_val);
          } catch (...) {}
          return;
        }
        auto topic = writeHost().ensureTopic("_info/" + key);
        if (!topic) {
          return;
        }
        auto ts_ns = static_cast<int64_t>(file_start_time_us) * 1000;
        (void)writeHost().appendRecord(*topic, PJ::Timestamp{ts_ns}, {{.name = "value", .value = val}});
      };

      for (const auto& [key, info] : data_container->messageInfo()) {
        write_info(key, info);
      }
      for (const auto& [key, values_vec] : data_container->messageInfoMulti()) {
        if (values_vec.empty() || values_vec[0].empty()) {
          continue;
        }
        write_info(key, values_vec[0][0]);  // first instance
      }
    }

    // Write embedded log messages as _log/ topic.
    {
      const auto& logs = data_container->logging();
      if (!logs.empty()) {
        auto topic = writeHost().ensureTopic("_log");
        if (topic) {
          (void)writeHost().ensureField(*topic, "level", PJ::PrimitiveType::kString);
          (void)writeHost().ensureField(*topic, "message", PJ::PrimitiveType::kString);
          for (const auto& log : logs) {
            auto ts_ns = static_cast<int64_t>(log.timestamp()) * 1000;
            std::string level_str = log.logLevelStr();
            std::string msg = log.message();
            (void)writeHost().appendRecord(
                *topic, PJ::Timestamp{ts_ns},
                {{.name = "level", .value = std::string_view(level_str)},
                 {.name = "message", .value = std::string_view(msg)}});
          }
        }
      }
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo, "Imported " + std::to_string(total_series_count) + " time series");

    return PJ::okStatus();
  }

 private:
  std::string filepath_;
  ulog_detail::ULogParamsDialog dialog_;
};

}  // namespace

PJ_DIALOG_PLUGIN(ulog_detail::ULogParamsDialog, kUlogManifest)
PJ_DATA_SOURCE_PLUGIN(ULogSource, kUlogManifest)
