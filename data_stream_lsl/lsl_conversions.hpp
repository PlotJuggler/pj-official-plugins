#pragma once

#include <lsl_cpp.h>

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/type_tree.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pj_lsl {

/// How LSL sample stamps become PlotJuggler absolute epoch-nanoseconds.
enum class TimestampMode {
  kSync,      ///< time_correction() + local_clock->epoch offset (default)
  kRaw,       ///< raw LSL stamp * 1e9 (parity with the PJ3 plugin)
  kReceiver,  ///< system clock at drain time
};

inline TimestampMode parseTimestampMode(std::string_view s) {
  if (s == "raw") {
    return TimestampMode::kRaw;
  }
  if (s == "receiver") {
    return TimestampMode::kReceiver;
  }
  return TimestampMode::kSync;  // default, including unknown values
}

inline const char* toString(TimestampMode mode) {
  switch (mode) {
    case TimestampMode::kRaw:
      return "raw";
    case TimestampMode::kReceiver:
      return "receiver";
    case TimestampMode::kSync:
    default:
      return "sync";
  }
}

/// LSL channel format -> SDK PrimitiveType. cf_undefined -> kUnspecified so the
/// caller can skip undecodable streams. LSL formats are per-stream, so all
/// channels of a stream share this type.
inline PJ::PrimitiveType mapChannelFormat(lsl::channel_format_t fmt) {
  switch (fmt) {
    case lsl::cf_float32:
      return PJ::PrimitiveType::kFloat32;
    case lsl::cf_double64:
      return PJ::PrimitiveType::kFloat64;
    case lsl::cf_int8:
      return PJ::PrimitiveType::kInt8;
    case lsl::cf_int16:
      return PJ::PrimitiveType::kInt16;
    case lsl::cf_int32:
      return PJ::PrimitiveType::kInt32;
    case lsl::cf_int64:
      return PJ::PrimitiveType::kInt64;
    case lsl::cf_string:
      return PJ::PrimitiveType::kString;
    case lsl::cf_undefined:
    default:
      return PJ::PrimitiveType::kUnspecified;
  }
}

inline bool isStringFormat(lsl::channel_format_t fmt) {
  return fmt == lsl::cf_string;
}

/// LSL sample stamp (seconds) -> absolute epoch nanoseconds, per mode.
/// `now_epoch_ns` is used by kReceiver and by the kSync s==0 fallback.
inline int64_t computeTimestampNs(
    TimestampMode mode, double sample_time_s, double time_correction_s, int64_t epoch_offset_ns, int64_t now_epoch_ns) {
  switch (mode) {
    case TimestampMode::kRaw:
      return static_cast<int64_t>(sample_time_s * 1e9);
    case TimestampMode::kReceiver:
      return now_epoch_ns;
    case TimestampMode::kSync:
    default:
      if (sample_time_s == 0.0) {
        return now_epoch_ns;  // inlet gave no stamp -> use arrival time
      }
      return static_cast<int64_t>((sample_time_s + time_correction_s) * 1e9) + epoch_offset_ns;
  }
}

/// Wrap a pulled double as a ValueRef of the field's native type. Integer
/// formats produce integer columns. (int64 values above 2^53 lose precision
/// through the double pull path — see spec future work.)
inline PJ::sdk::ValueRef numericValueRef(PJ::PrimitiveType type, double v) {
  switch (type) {
    case PJ::PrimitiveType::kFloat32:
      return static_cast<float>(v);
    case PJ::PrimitiveType::kInt8:
      return static_cast<int8_t>(v);
    case PJ::PrimitiveType::kInt16:
      return static_cast<int16_t>(v);
    case PJ::PrimitiveType::kInt32:
      return static_cast<int32_t>(v);
    case PJ::PrimitiveType::kInt64:
      return static_cast<int64_t>(v);
    case PJ::PrimitiveType::kFloat64:
    default:
      return v;
  }
}

struct StreamKey {
  std::string name;
  std::string source_id;
};

/// Per-stream topic names, unique and order-preserving. Unique names pass
/// through; collisions get " (source_id)", or " #<n>" when source_id is empty.
inline std::vector<std::string> uniqueTopicNames(const std::vector<StreamKey>& streams) {
  std::map<std::string, int> name_counts;
  for (const auto& s : streams) {
    ++name_counts[s.name];
  }
  std::map<std::string, int> dup_index;
  std::vector<std::string> out;
  out.reserve(streams.size());
  for (const auto& s : streams) {
    if (name_counts[s.name] <= 1) {
      out.push_back(s.name);
      continue;
    }
    if (!s.source_id.empty()) {
      out.push_back(s.name + " (" + s.source_id + ")");
    } else {
      out.push_back(s.name + " #" + std::to_string(dup_index[s.name]++));
    }
  }
  return out;
}

/// Channel labels from the stream's XML desc, with "channel_<i>" fallback.
/// Always returns channel_count() entries. Copies the stream_info because
/// lsl::stream_info::desc() is non-const.
inline std::vector<std::string> channelLabels(const lsl::stream_info& info) {
  const int n = info.channel_count();
  lsl::stream_info info_copy = info;  // desc() is non-const; copy is a deep copy
  lsl::xml_element channels = info_copy.desc().child("channels");
  lsl::xml_element ch = channels.empty() ? lsl::xml_element() : channels.child("channel");

  std::vector<std::string> labels;
  labels.reserve(static_cast<size_t>(n < 0 ? 0 : n));
  for (int i = 0; i < n; ++i) {
    std::string label;
    if (!ch.empty()) {
      const char* v = ch.child_value("label");
      if (v != nullptr && v[0] != '\0') {
        label = v;
      }
      ch = ch.next_sibling("channel");
    }
    if (label.empty()) {
      label = "channel_" + std::to_string(i);
    }
    labels.push_back(std::move(label));
  }
  return labels;
}

/// A user-selected stream's persistent identity (never the per-session uid).
struct SelectedStream {
  std::string source_id;
  std::string name;
  std::string type;
};

struct DialogConfig {
  std::vector<SelectedStream> streams;
  TimestampMode mode = TimestampMode::kSync;
};

inline std::string serializeConfig(const DialogConfig& cfg) {
  nlohmann::json j;
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& s : cfg.streams) {
    arr.push_back({{"source_id", s.source_id}, {"name", s.name}, {"type", s.type}});
  }
  j["streams"] = arr;
  j["timestamp_mode"] = toString(cfg.mode);
  return j.dump();
}

inline DialogConfig parseConfig(std::string_view json) {
  DialogConfig cfg;
  // Config comes from saved layouts and may be hand-edited/corrupted. Guard the
  // shape explicitly and catch nlohmann type_errors so nothing propagates across
  // the plugin's loadConfig() boundary; a malformed config just yields defaults.
  try {
    auto j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      return {};  // defaults: no streams, sync mode
    }
    if (j.contains("streams") && j["streams"].is_array()) {
      for (const auto& e : j["streams"]) {
        if (!e.is_object()) {
          continue;
        }
        cfg.streams.push_back(
            {e.value("source_id", std::string{}), e.value("name", std::string{}), e.value("type", std::string{})});
      }
    }
    if (j.contains("timestamp_mode") && j["timestamp_mode"].is_string()) {
      cfg.mode = parseTimestampMode(j["timestamp_mode"].get<std::string>());
    }
  } catch (const std::exception&) {
    return {};  // any type/parse error -> safe defaults
  }
  return cfg;
}

}  // namespace pj_lsl
