#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

/// How one leaf of a record becomes one cell of the row handed to the host.
struct LeafPlan {
  enum class Kind : uint8_t { kCopy, kBool, kString };
  size_t offset;
  size_t size;
  Kind kind;
};

/// Everything about a subscription's rows that does not change from record to
/// record, worked out once: where each leaf sits, and a row whose field handles
/// and value types are already filled in. Decoding a record is then one pass
/// that patches the values in place, with no format walk, no intermediate
/// ValueRef and no per-record allocation.
struct RowPlan {
  std::vector<LeafPlan> leaves;
  std::vector<PJ_bound_field_value_t> row;

  RowPlan() = default;

  RowPlan(const ulog_cpp::MessageFormat& format, const std::vector<PJ::sdk::FieldHandle>& handles) {
    static constexpr uint8_t kZero[8] = {};
    ulog_flatten::forEachFlatLeaf(format, 0, [&](const ulog_flatten::FlatLeaf& leaf) {
      if (leaves.size() == handles.size()) {
        return;
      }
      // The zero value only supplies the ABI type tag; decode() writes the data.
      const PJ::sdk::ValueRef zero =
          leaf.is_string ? PJ::sdk::ValueRef{std::string_view{}} : readPrimitiveValue(kZero, 0, leaf.type);
      const auto kind = leaf.is_string                                  ? LeafPlan::Kind::kString
                        : leaf.type == ulog_cpp::Field::BasicType::BOOL ? LeafPlan::Kind::kBool
                                                                        : LeafPlan::Kind::kCopy;
      row.push_back({.field = handles[leaves.size()], .is_null = false, .value = PJ::sdk::toAbiScalar(zero)});
      leaves.push_back({leaf.offset, leaf.size, kind});
    });
  }

  /// Fill `row` from one raw record. A leaf that falls outside `raw_size` (a
  /// truncated or corrupt record) becomes null rather than an out-of-bounds read.
  void decode(const uint8_t* raw, size_t raw_size) {
    for (size_t i = 0; i < leaves.size(); ++i) {
      const LeafPlan& leaf = leaves[i];
      PJ_bound_field_value_t& cell = row[i];
      cell.is_null = leaf.offset + leaf.size > raw_size;
      if (cell.is_null) {
        continue;
      }
      switch (leaf.kind) {
        case LeafPlan::Kind::kCopy:
          // ULog scalars are little-endian and at most 8 bytes; every member of
          // the value union starts at its first byte.
          std::memcpy(&cell.value.data, raw + leaf.offset, leaf.size);
          break;
        case LeafPlan::Kind::kBool:
          cell.value.data.as_bool = raw[leaf.offset] != 0 ? 1 : 0;
          break;
        case LeafPlan::Kind::kString:
          cell.value.data.as_string = PJ::sdk::toAbiString(ulog_flatten::stringLeafView(raw, leaf.offset, leaf.size));
          break;
      }
    }
  }
};

/// Joins up to `max_shown` names, then "... (+N more)".
std::string sampleList(const std::vector<std::string>& names, size_t max_shown) {
  std::string out;
  for (size_t i = 0; i < names.size() && i < max_shown; ++i) {
    out += (i ? ", " : "") + names[i];
  }
  if (names.size() > max_shown) {
    out += ", ... (+" + std::to_string(names.size() - max_shown) + " more)";
  }
  return out;
}

/// Reads `path` through `container` in 64 KiB chunks. `after_chunk(bytes_read)`
/// returns false to stop early.
template <typename AfterChunk>
bool readLog(
    const std::string& path, const std::shared_ptr<ulog_cpp::DataContainer>& container, AfterChunk&& after_chunk) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  ulog_cpp::Reader reader{container};
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
    if (!after_chunk(bytes_read)) {
      break;
    }
  }
  return true;
}

/// Writes every data record to the host as the reader hands it over, instead
/// of keeping the whole log in memory and writing it afterwards. Nothing but
/// the record being decoded is held, so peak memory no longer grows with the
/// file, and the bytes are decoded while they are still in cache.
class StreamingContainer : public ulog_container::ULogContainer {
 public:
  /// `max_multi_id` decides which topic names carry a `.NN` instance suffix. It
  /// has to be known before the first record is written, hence the first pass.
  StreamingContainer(PJ::sdk::SourceWriteHostView write_host, std::map<std::string, int> max_multi_id)
      : ULogContainer(false), write_host_(write_host), max_multi_id_(std::move(max_multi_id)) {}

  void data(const ulog_cpp::Data& data_msg) override {
    // Advances the parameter clock, and throws on an unknown subscription.
    ULogContainer::data(data_msg);
    if (!error_.empty()) {
      return;
    }

    const size_t id = data_msg.msgId();
    if (id >= topics_.size()) {
      topics_.resize(id + 1);
    }
    if (!topics_[id]) {
      topics_[id] = openTopic(data_msg.msgId());
    }
    Topic& topic = *topics_[id];
    const uint64_t sample_index = topic.samples_seen++;
    if (!topic.usable) {
      return;
    }

    // ulog_cpp validates the message FRAME, not the payload against the
    // format. A record shorter than its format is corrupt: skip it rather
    // than fabricate a timestamp and null fields for it.
    const auto& raw = data_msg.data();
    if (raw.size() < topic.format_size) {
      if (short_records_++ == 0) {
        first_short_record_topic_ = topic.name;
      }
      return;
    }

    // Timestamp is uint64_t microseconds; a format without one falls back to
    // the sample index, exactly as the original DataLoadULog did.
    uint64_t timestamp_us = sample_index;
    if (topic.ts_offset) {
      std::memcpy(&timestamp_us, raw.data() + *topic.ts_offset, sizeof(timestamp_us));
    }
    const auto ts_ns = static_cast<int64_t>(timestamp_us) * 1000;

    topic.plan.decode(raw.data(), raw.size());
    const PJ_source_write_host_t& host = write_host_.raw();
    if (!host.vtable->append_bound_record(
            host.ctx, topic.handle, ts_ns, topic.plan.row.data(), topic.plan.row.size(), &host_error_)) {
      error_ = PJ::sdk::errorToString(host_error_);
    }
  }

  /// First host error; once set, no further record is written.
  const std::string& writeError() const {
    return error_;
  }
  size_t seriesCount() const {
    return series_count_;
  }
  const std::vector<std::string>& topicsWithoutTimestamp() const {
    return topics_without_timestamp_;
  }
  size_t shortRecords() const {
    return short_records_;
  }
  const std::string& firstShortRecordTopic() const {
    return first_short_record_topic_;
  }

 private:
  struct Topic {
    bool usable = false;
    std::string name;
    PJ::sdk::TopicHandle handle{};
    RowPlan plan;
    std::optional<size_t> ts_offset;
    size_t format_size = 0;
    uint64_t samples_seen = 0;
  };

  /// Creates the host topic and its fields on a subscription's first record,
  /// so a subscription that never logs anything leaves no empty topic behind.
  std::unique_ptr<Topic> openTopic(uint16_t msg_id) {
    auto topic = std::make_unique<Topic>();
    const auto& sub = subscriptionsByMessageId().at(msg_id);
    const auto& format = *sub->format();

    // Build topic name, adding multi_id suffix when needed.
    const auto& added = sub->getAddLoggedMessage();
    topic->name = added.messageName();
    auto max_it = max_multi_id_.find(topic->name);
    if (max_it != max_multi_id_.end() && max_it->second > 0) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), ".%02d", static_cast<int>(added.multiId()));
      topic->name += buf;
    }

    std::vector<std::string> field_names;
    ulog_flatten::collectFlatFieldNames(format, {}, field_names);
    if (field_names.empty()) {
      return topic;
    }

    auto handle = write_host_.ensureTopic(topic->name);
    if (!handle) {
      error_ = handle.error();
      return topic;
    }
    topic->handle = *handle;

    std::vector<PJ::PrimitiveType> field_types;
    ulog_flatten::forEachFlatLeaf(format, 0, [&](const ulog_flatten::FlatLeaf& leaf) {
      field_types.push_back(leaf.is_string ? PJ::PrimitiveType::kString : ulogTypeToPrimitive(leaf.type));
    });

    // Rows are appended by handle: the by-name path copies every field name
    // into each row and makes the host re-resolve it, per field per record.
    std::vector<PJ::sdk::FieldHandle> field_handles;
    field_handles.reserve(field_names.size());
    for (size_t fi = 0; fi < field_names.size() && fi < field_types.size(); ++fi) {
      auto field = write_host_.ensureField(*handle, field_names[fi], field_types[fi]);
      if (!field) {
        error_ = field.error();
        return topic;
      }
      field_handles.push_back(*field);
    }

    // The spec mandates a `uint64_t timestamp` field but not its position, so
    // locate it by name (original plugin issue #1154 for formats without one).
    topic->ts_offset = ulog_flatten::findTimestampOffset(format);
    if (!topic->ts_offset) {
      topics_without_timestamp_.push_back(topic->name);
    }
    // A trailing `_padding*` field is not required to be present in the logged
    // bytes (ULog spec, "Padding"), so the minimum valid payload size excludes
    // it; padding anywhere else still counts (see ulog_flatten::loggedSizeBytes).
    topic->format_size = ulog_flatten::loggedSizeBytes(format);
    topic->plan = RowPlan(format, field_handles);
    topic->usable = true;
    series_count_ += field_names.size();
    return topic;
  }

  PJ::sdk::SourceWriteHostView write_host_;
  std::map<std::string, int> max_multi_id_;
  std::vector<std::unique_ptr<Topic>> topics_;  // indexed by msg_id
  PJ_error_t host_error_{};
  std::string error_;
  size_t series_count_ = 0;
  // Diagnostics are aggregated and reported once: a malformed log can define
  // thousands of subscriptions, and one message per problem would flood the
  // host (whose dedup is by exact text).
  std::vector<std::string> topics_without_timestamp_;
  size_t short_records_ = 0;
  std::string first_short_record_topic_;
};

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

    std::error_code size_error;
    const auto file_size = static_cast<uint64_t>(std::filesystem::file_size(filepath_, size_error));

    (void)runtimeHost().progressStart("Importing ULog", size_error ? 0 : file_size, true);

    // First pass: subscriptions only. A topic name carries a `.NN` suffix when
    // any instance of it has multi_id > 0, and instances can be added anywhere
    // in the file, so the names must be settled before the first record is written.
    std::map<std::string, int> max_multi_id;
    {
      auto scan = std::make_shared<ulog_container::ULogContainer>(false);
      if (!readLog(filepath_, scan, [&](uint64_t) { return !runtimeHost().isStopRequested(); })) {
        return PJ::unexpected(std::string("cannot open file: ") + filepath_);
      }
      for (const auto& [key, sub] : scan->subscriptionsByNameAndMultiId()) {
        auto [it, inserted] = max_multi_id.try_emplace(key.name, key.multi_id);
        it->second = std::max(it->second, static_cast<int>(key.multi_id));
      }
    }

    // Second pass: decode and write each record as it is read. A stop request
    // ends the read; everything written up to there is already with the host
    // ("Stop and Keep" flushes it, Discard evicts the dataset host-side).
    auto data_container = std::make_shared<StreamingContainer>(writeHost(), std::move(max_multi_id));
    readLog(filepath_, data_container, [&](uint64_t bytes_read) {
      if (!data_container->writeError().empty() || runtimeHost().isStopRequested()) {
        return false;
      }
      (void)runtimeHost().progressUpdate(bytes_read);
      return true;
    });
    if (!data_container->writeError().empty()) {
      return PJ::unexpected(data_container->writeError());
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

    const size_t total_series_count = data_container->seriesCount();
    const auto& topics_without_timestamp = data_container->topicsWithoutTimestamp();
    const size_t short_records = data_container->shortRecords();
    const auto& first_short_record_topic = data_container->firstShortRecordTopic();
    auto sample_list = sampleList;

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
