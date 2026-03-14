#include <pj_base/sdk/data_source_patterns.hpp>

#include <ulog_cpp/data_container.hpp>
#include <ulog_cpp/reader.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

/// Recursively collect flattened field names for a ulog_cpp MessageFormat.
/// Skips the "timestamp" field and "_padding*" fields.
/// Nested fields are separated by "." and array elements get ".00", ".01" etc.
void collectFlatFieldNames(const ulog_cpp::MessageFormat& format, const std::string& prefix,
                           std::vector<std::string>& out) {
  for (const auto& field_ptr : format.fields()) {
    const auto& field = *field_ptr;
    const std::string& name = field.name();

    // Skip timestamp and padding fields.
    if (name == "timestamp") {
      continue;
    }
    if (name.substr(0, 8) == "_padding") {
      continue;
    }

    std::string new_prefix = prefix.empty() ? name : prefix + "." + name;

    int arr_len = field.arrayLength();
    int count = (arr_len < 0) ? 1 : arr_len;
    bool is_array = (arr_len > 0);

    for (int i = 0; i < count; ++i) {
      std::string elem_name = new_prefix;
      if (is_array) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), ".%02d", i);
        elem_name += buf;
      }

      if (field.type().type == ulog_cpp::Field::BasicType::NESTED) {
        auto nested_fmt = field.nestedFormat();
        if (nested_fmt) {
          collectFlatFieldNames(*nested_fmt, elem_name, out);
        }
      } else {
        out.push_back(elem_name);
      }
    }
  }
}

/// Read a single primitive value from raw ULog data at a given offset.
double readPrimitiveValue(const uint8_t* data, size_t offset, ulog_cpp::Field::BasicType type) {
  const uint8_t* p = data + offset;
  switch (type) {
    case ulog_cpp::Field::BasicType::INT8:
      return static_cast<double>(*reinterpret_cast<const int8_t*>(p));
    case ulog_cpp::Field::BasicType::UINT8:
      return static_cast<double>(*p);
    case ulog_cpp::Field::BasicType::INT16: {
      int16_t v;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case ulog_cpp::Field::BasicType::UINT16: {
      uint16_t v;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case ulog_cpp::Field::BasicType::INT32: {
      int32_t v;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case ulog_cpp::Field::BasicType::UINT32: {
      uint32_t v;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case ulog_cpp::Field::BasicType::INT64: {
      int64_t v;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case ulog_cpp::Field::BasicType::UINT64: {
      uint64_t v;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case ulog_cpp::Field::BasicType::FLOAT: {
      float v;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case ulog_cpp::Field::BasicType::DOUBLE: {
      double v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case ulog_cpp::Field::BasicType::BOOL:
      return static_cast<double>(*p);
    case ulog_cpp::Field::BasicType::CHAR:
      return static_cast<double>(*p);
    default:
      return 0.0;
  }
}

/// Recursively extract numeric values from raw data bytes using resolved field offsets,
/// matching the order produced by collectFlatFieldNames.
void extractFlatValues(const uint8_t* raw_data, size_t base_offset,
                       const ulog_cpp::MessageFormat& format, std::vector<double>& values) {
  for (const auto& field_ptr : format.fields()) {
    const auto& field = *field_ptr;
    const std::string& name = field.name();

    if (name == "timestamp") {
      continue;
    }
    if (name.substr(0, 8) == "_padding") {
      continue;
    }

    int arr_len = field.arrayLength();
    int count = (arr_len < 0) ? 1 : arr_len;
    size_t field_offset = base_offset + static_cast<size_t>(field.offsetInMessage());

    if (field.type().type == ulog_cpp::Field::BasicType::NESTED) {
      auto nested_fmt = field.nestedFormat();
      if (!nested_fmt) {
        continue;
      }
      size_t nested_size = nested_fmt->sizeBytes();
      for (int i = 0; i < count; ++i) {
        extractFlatValues(raw_data, field_offset + static_cast<size_t>(i) * nested_size, *nested_fmt,
                          values);
      }
    } else {
      size_t elem_size = field.type().size;
      for (int i = 0; i < count; ++i) {
        values.push_back(
            readPrimitiveValue(raw_data, field_offset + static_cast<size_t>(i) * elem_size, field.type().type));
      }
    }
  }
}

class ULogSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override { return PJ::kCapabilityDirectIngest; }

  std::string saveConfig() const override {
    return nlohmann::json{{"filepath", filepath_}}.dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    filepath_ = cfg.value("filepath", std::string{});
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

    // Parse via ulog_cpp.
    auto data_container =
        std::make_shared<ulog_cpp::DataContainer>(ulog_cpp::DataContainer::StorageConfig::FullLog);
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
        return PJ::unexpected(std::string("import cancelled"));
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
      collectFlatFieldNames(*sub->format(), {}, field_names);
      if (field_names.empty()) {
        continue;
      }

      // Ensure topic and pre-register all fields.
      auto topic = writeHost().ensureTopic(topic_name);
      if (!topic) {
        return PJ::unexpected(topic.error());
      }

      for (const auto& fname : field_names) {
        auto field = writeHost().ensureField(*topic, fname, PJ::PrimitiveType::kFloat64);
        if (!field) {
          return PJ::unexpected(field.error());
        }
      }

      // Write data records.
      std::vector<PJ::sdk::NamedFieldValue> row_fields;
      row_fields.reserve(field_names.size());
      std::vector<double> values;
      values.reserve(field_names.size());

      const auto& raw_samples = sub->rawSamples();
      for (size_t i = 0; i < raw_samples.size(); ++i) {
        const auto& raw = raw_samples[i].data();

        // Extract timestamp (first 8 bytes, uint64_t microseconds) and convert to nanoseconds.
        uint64_t timestamp_us = 0;
        if (raw.size() >= 8) {
          std::memcpy(&timestamp_us, raw.data(), sizeof(timestamp_us));
        }
        auto ts_ns = static_cast<int64_t>(timestamp_us) * 1000;

        // Extract all field values from raw bytes.
        values.clear();
        extractFlatValues(raw.data(), 0, *sub->format(), values);

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

    // Write parameters as single-point timeseries.
    for (const auto& [param_name, param] : data_container->initialParameters()) {
      std::string ptopic_name = "_parameters/" + param_name;
      auto topic = writeHost().ensureTopic(ptopic_name);
      if (!topic) {
        return PJ::unexpected(topic.error());
      }

      double param_value = 0.0;
      try {
        param_value = param.value().as<double>();
      } catch (const std::exception&) {
        continue;  // skip non-numeric parameters
      }

      auto ts_ns = static_cast<int64_t>(file_start_time_us) * 1000;
      auto status =
          writeHost().appendRecord(*topic, PJ::Timestamp{ts_ns}, {{.name = "value", .value = param_value}});
      if (!status) {
        return status;
      }
    }

    runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo,
                                "Imported " + std::to_string(total_series_count) + " time series");

    return PJ::okStatus();
  }

 private:
  std::string filepath_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(ULogSource, R"({"name":"ULog File Source","version":"1.0.0","file_extensions":[".ulg"]})")
