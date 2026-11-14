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
#include <pj_base/builtin/log_codec.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <set>
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

/// ULog logged-message level ('0'..'7', syslog-style) -> canonical log level.
PJ::sdk::Log::Level toLogLevel(ulog_cpp::Logging::Level level) {
  using L = ulog_cpp::Logging::Level;
  switch (level) {
    case L::Emergency:
    case L::Alert:
    case L::Critical:
      return PJ::sdk::Log::Level::kFatal;
    case L::Error:
      return PJ::sdk::Log::Level::kError;
    case L::Warning:
      return PJ::sdk::Log::Level::kWarning;
    case L::Notice:
    case L::Info:
      return PJ::sdk::Log::Level::kInfo;
    case L::Debug:
      return PJ::sdk::Log::Level::kDebug;
  }
  return PJ::sdk::Log::Level::kUnknown;
}

/// How one leaf of a record becomes one cell of the row handed to the host.
/// ULog scalars are 1, 2, 4 or 8 bytes; one kind per width keeps each copy a
/// fixed-size move instead of a call into memcpy.
struct LeafPlan {
  enum class Kind : uint8_t { kCopy1, kCopy2, kCopy4, kCopy8, kBool, kString };
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
  std::vector<PJ::PrimitiveType> types;  // one per leaf, same order
  std::vector<PJ_bound_field_value_t> row;

  RowPlan() = default;

  explicit RowPlan(const ulog_cpp::MessageFormat& format) {
    ulog_flatten::forEachFlatLeaf(format, 0, [&](const ulog_flatten::FlatLeaf& leaf) {
      auto kind = LeafPlan::Kind::kCopy8;
      if (leaf.is_string) {
        kind = LeafPlan::Kind::kString;
      } else if (leaf.type == ulog_cpp::Field::BasicType::BOOL) {
        kind = LeafPlan::Kind::kBool;
      } else if (leaf.size == 1) {
        kind = LeafPlan::Kind::kCopy1;
      } else if (leaf.size == 2) {
        kind = LeafPlan::Kind::kCopy2;
      } else if (leaf.size == 4) {
        kind = LeafPlan::Kind::kCopy4;
      }
      leaves.push_back({leaf.offset, leaf.size, kind});
      types.push_back(leaf.is_string ? PJ::PrimitiveType::kString : ulogTypeToPrimitive(leaf.type));
    });
  }

  /// Build the row once the host has handed out a field handle per leaf.
  void bind(const std::vector<PJ::sdk::FieldHandle>& handles) {
    row.clear();
    for (size_t i = 0; i < leaves.size(); ++i) {
      PJ_bound_field_value_t cell{};
      cell.field = handles[i];
      cell.value.type = PJ::sdk::toAbiType(types[i]);
      row.push_back(cell);
    }
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
      // ULog scalars are little-endian, and every member of the value union
      // starts at its first byte, so copying the leaf's bytes sets the member.
      const uint8_t* src = raw + leaf.offset;
      switch (leaf.kind) {
        case LeafPlan::Kind::kCopy1:
          std::memcpy(&cell.value.data, src, 1);
          break;
        case LeafPlan::Kind::kCopy2:
          std::memcpy(&cell.value.data, src, 2);
          break;
        case LeafPlan::Kind::kCopy4:
          std::memcpy(&cell.value.data, src, 4);
          break;
        case LeafPlan::Kind::kCopy8:
          std::memcpy(&cell.value.data, src, 8);
          break;
        case LeafPlan::Kind::kBool:
          cell.value.data.as_bool = *src != 0 ? 1 : 0;
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

/// Writes every data record to the host as the reader hands it over, instead
/// of keeping the whole log in memory and writing it afterwards. Nothing but
/// the record being decoded is held, so peak memory no longer grows with the
/// file, and the bytes are decoded while they are still in cache.
class StreamingContainer : public ulog_container::ULogContainer {
 public:
  /// `multi_instance` holds the message names whose topics carry a `.NN`
  /// instance suffix. It has to be known before the first record is written, hence the first pass.
  StreamingContainer(PJ::sdk::SourceWriteHostView write_host, std::set<std::string> multi_instance)
      : ULogContainer(false), write_host_(write_host), multi_instance_(std::move(multi_instance)) {}

  void data(const ulog_cpp::Data& data_msg) override {
    // Advances the parameter clock, and throws on an unknown subscription.
    ULogContainer::data(data_msg);
    if (!result.write_error.empty()) {
      return;
    }

    const size_t id = data_msg.msgId();
    if (id >= topics_.size()) {
      topics_.resize(id + 1);
    }
    if (!topics_[id]) {
      // A format ulog_cpp could not resolve throws from here. Left to escape,
      // the reader would take it for file corruption and resync on every
      // record: fail the import instead, and never retry this subscription.
      try {
        topics_[id] = openTopic(data_msg.msgId());
      } catch (const std::exception& e) {
        result.write_error = e.what();
        topics_[id] = std::make_unique<Topic>();
        return;
      }
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
    if (raw.size() < topic.min_size) {
      if (result.short_records++ == 0) {
        result.first_short_record_topic = topic.name;
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
    // Straight to the host's C entry point: the SDK's appendBoundRecord takes
    // ValueRef cells and rebuilds this row in a fresh vector on every call. An
    // SDK overload accepting a ready-made row would let this go back through it.
    const PJ_source_write_host_t& host = write_host_.raw();
    if (!host.vtable->append_bound_record(
            host.ctx, topic.handle, ts_ns, topic.plan.row.data(), topic.plan.row.size(), &host_error_)) {
      result.write_error = PJ::sdk::errorToString(host_error_);
    }
  }

  /// What the import has to report once the file is read.
  struct Result {
    std::string write_error;  // first host error; once set, no further record is written
    size_t series_count = 0;
    // Diagnostics are aggregated and reported once: a malformed log can define
    // thousands of subscriptions, and one message per problem would flood the
    // host (whose dedup is by exact text).
    std::vector<std::string> topics_without_timestamp;
    size_t short_records = 0;
    std::string first_short_record_topic;
  };
  Result result;

 private:
  struct Topic {
    bool usable = false;
    std::string name;
    PJ::sdk::TopicHandle handle{};
    RowPlan plan;
    std::optional<size_t> ts_offset;
    size_t min_size = 0;  // shortest payload that holds every logged field and the timestamp
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

    // ulog_cpp keeps the FIRST subscription of a (name, multi_id) pair in its
    // by-name table; a later duplicate under another msg_id was never imported,
    // and must not be merged into the first one's topic now.
    const auto& by_name = subscriptionsByNameAndMultiId();
    const auto first = by_name.find({topic->name, added.multiId()});
    if (first == by_name.end() || first->second != sub) {
      return topic;
    }

    if (multi_instance_.count(topic->name) != 0) {
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
      result.write_error = handle.error();
      return topic;
    }
    topic->handle = *handle;

    // Rows are appended by handle: the by-name path copies every field name
    // into each row and makes the host re-resolve it, per field per record.
    // collectFlatFieldNames and forEachFlatLeaf visit the leaves in the same order.
    topic->plan = RowPlan(format);
    if (topic->plan.leaves.size() != field_names.size()) {
      return topic;
    }
    std::vector<PJ::sdk::FieldHandle> field_handles;
    field_handles.reserve(field_names.size());
    for (size_t fi = 0; fi < field_names.size(); ++fi) {
      auto field = write_host_.ensureField(*handle, field_names[fi], topic->plan.types[fi]);
      if (!field) {
        result.write_error = field.error();
        return topic;
      }
      field_handles.push_back(*field);
    }
    topic->plan.bind(field_handles);

    // The spec mandates a `uint64_t timestamp` field but not its position, so
    // locate it by name (original plugin issue #1154 for formats without one).
    topic->ts_offset = ulog_flatten::findTimestampOffset(format);
    if (!topic->ts_offset) {
      result.topics_without_timestamp.push_back(topic->name);
    }
    // A trailing `_padding*` field is not required to be present in the logged
    // bytes (ULog spec, "Padding"), so the minimum valid payload size excludes
    // it; padding anywhere else still counts (see ulog_flatten::loggedSizeBytes).
    // That size under-counts a format with duplicate field names, so the
    // timestamp is covered explicitly: it is read before any bounds-checked leaf.
    topic->min_size = ulog_flatten::loggedSizeBytes(format);
    if (topic->ts_offset) {
      topic->min_size = std::max(topic->min_size, *topic->ts_offset + sizeof(uint64_t));
    }
    topic->usable = true;
    result.series_count += field_names.size();
    return topic;
  }

  PJ::sdk::SourceWriteHostView write_host_;
  std::set<std::string> multi_instance_;
  std::vector<std::unique_ptr<Topic>> topics_;  // indexed by msg_id
  PJ_error_t host_error_{};                     // a member: zeroing it per record would cost more than the decode
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
    const auto file_size = std::max<uint64_t>(std::filesystem::file_size(filepath_, size_error), 1);

    // One bar over both passes. The first only parses, so it gets a small share;
    // the second also writes every record to the host, which is where the time goes.
    static constexpr uint64_t kProgressSteps = 1000;
    static constexpr uint64_t kScanSteps = 50;
    const auto steps = [&](uint64_t bytes_read, uint64_t span) {
      return std::min(bytes_read, file_size) * span / file_size;
    };
    (void)runtimeHost().progressStart("Importing ULog", kProgressSteps, true);

    // First pass: subscriptions only. A topic name carries a `.NN` suffix when
    // any instance of it has multi_id > 0, and instances can be added anywhere
    // in the file, so the names must be settled before the first record is written.
    std::set<std::string> multi_instance;
    uint64_t scanned_bytes = 0;
    bool stopped_in_scan = false;
    {
      auto scan = std::make_shared<ulog_container::ULogContainer>(false);
      const bool opened = ulog_container::readLog(filepath_, scan, [&](uint64_t bytes_read) {
        scanned_bytes = bytes_read;
        stopped_in_scan = runtimeHost().isStopRequested();
        (void)runtimeHost().progressUpdate(steps(bytes_read, kScanSteps));
        return !stopped_in_scan;
      });
      if (!opened) {
        return PJ::unexpected(std::string("cannot open file: ") + filepath_);
      }
      for (const auto& [key, sub] : scan->subscriptionsByNameAndMultiId()) {
        if (key.multi_id > 0) {
          multi_instance.insert(key.name);
        }
      }
    }

    // Second pass: decode and write each record as it is read. A stop request
    // ends the read; everything written up to there is already with the host
    // ("Stop and Keep" flushes it, Discard evicts the dataset host-side). A stop
    // that came during the first pass keeps the part of the file read by then.
    auto data_container = std::make_shared<StreamingContainer>(writeHost(), std::move(multi_instance));
    const auto& result = data_container->result;
    const bool reopened = ulog_container::readLog(filepath_, data_container, [&](uint64_t bytes_read) {
      if (!result.write_error.empty()) {
        return false;
      }
      if (stopped_in_scan) {
        return bytes_read < scanned_bytes;
      }
      (void)runtimeHost().progressUpdate(kScanSteps + steps(bytes_read, kProgressSteps - kScanSteps));
      return !runtimeHost().isStopRequested();
    });
    if (!reopened) {
      return PJ::unexpected(std::string("cannot open file: ") + filepath_);
    }
    if (!result.write_error.empty()) {
      return PJ::unexpected(result.write_error);
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

    if (!result.topics_without_timestamp.empty()) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "ULog: " + std::to_string(result.topics_without_timestamp.size()) +
              " message format(s) have no timestamp field; the sample index is used as time: " +
              sampleList(result.topics_without_timestamp, 5));
    }
    if (result.short_records > 0) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning, "ULog: skipped " + std::to_string(result.short_records) +
                                                    " data record(s) shorter than their message format (first in '" +
                                                    result.first_short_record_topic + "')");
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
              " non-numeric parameter message(s) not plotted: " + sampleList(non_numeric_params, 5));
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
        writeLogObjects(logs);
      }
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo, "Imported " + std::to_string(result.series_count) + " time series");

    return PJ::okStatus();
  }

 private:
  // The same log messages as kLog objects for the log viewer, on an object topic
  // also named `_log` (the object store and the scalar store are separate, like
  // a parser's /rosout). Skipped with a warning when the host has no object store.
  void writeLogObjects(const std::vector<ulog_cpp::Logging>& logs) {
    const auto* owh = objectWriteHost();
    if (owh == nullptr) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "ULog: ObjectStore write host unavailable; log messages imported as scalars only");
      return;
    }
    auto topic = owh->registerTopic("_log", PJ::sdk::BuiltinObjectType::kLog);
    if (!topic) {
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, "ULog: log objects skipped: " + topic.error());
      return;
    }
    for (const auto& log : logs) {
      PJ::sdk::Log entry;
      entry.timestamp_ns = static_cast<int64_t>(log.timestamp()) * 1000;
      entry.level = toLogLevel(log.logLevel());
      entry.message = log.message();
      if (log.hasTag()) {
        entry.category = std::to_string(log.tag());
      }
      const auto payload = PJ::serializeLog(entry);
      if (auto status = owh->pushOwned(
              *topic, PJ::Timestamp{entry.timestamp_ns}, PJ::Span<const uint8_t>(payload.data(), payload.size()));
          !status) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning, "ULog: log objects stopped: " + status.error());
        return;
      }
    }
  }

  std::string filepath_;
  ulog_detail::ULogParamsDialog dialog_;
};

}  // namespace

PJ_DIALOG_PLUGIN(ulog_detail::ULogParamsDialog, kUlogManifest)
PJ_DATA_SOURCE_PLUGIN(ULogSource, kUlogManifest)
