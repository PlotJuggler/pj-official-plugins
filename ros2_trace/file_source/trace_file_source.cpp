#include <cstdint>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

#include "ros2_trace_model/fs_trace_source.hpp"
#include "ros2_trace_model/pipeline.hpp"
#include "ros2_trace_model/sinks.hpp"
#include "trace_manifest.hpp"

namespace {

// If the user picked the trace's "metadata" file, use its parent directory;
// otherwise treat the path as the trace directory itself.
std::string resolveTraceDir(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  const std::string leaf = (slash == std::string::npos) ? path : path.substr(slash + 1);
  if (leaf == "metadata" && slash != std::string::npos) {
    return path.substr(0, slash);
  }
  return path;
}

// MetricSampleSink that writes each derived sample to the PlotJuggler datastore.
// The series path is split into a topic (everything up to the last '/') and a
// field (the leaf); PlotJuggler re-joins them with '/' to reproduce the path.
class WriteHostSink : public ros2_trace_model::MetricSampleSink {
 public:
  explicit WriteHostSink(const PJ::sdk::SourceWriteHostView& host) : host_(host) {}

  void onSample(const ros2_trace_model::Sample& sample) override {
    if (!first_error_.empty()) {
      return;  // a previous write already failed; stop touching the host
    }

    const auto slash = sample.series.find_last_of('/');
    const std::string topic_name =
        (slash == std::string::npos) ? std::string("ros2_trace") : sample.series.substr(0, slash);
    const std::string field_name = (slash == std::string::npos) ? sample.series : sample.series.substr(slash + 1);

    PJ::sdk::TopicHandle topic;
    if (const auto it = topics_.find(topic_name); it != topics_.end()) {
      topic = it->second;
    } else {
      auto resolved = host_.ensureTopic(topic_name);
      if (!resolved) {
        first_error_ = resolved.error();
        return;
      }
      topic = *resolved;
      topics_.emplace(topic_name, topic);
    }

    PJ::sdk::NamedFieldValue field;
    field.name = field_name;
    // Push native ValueRef types (never cast to double): numeric stays double,
    // categorical stays string. The string_view is valid for the appendRecord call.
    std::string scratch;
    if (std::holds_alternative<double>(sample.value)) {
      field.value = std::get<double>(sample.value);
    } else {
      scratch = std::get<std::string>(sample.value);
      field.value = std::string_view(scratch);
    }

    const auto status =
        host_.appendRecord(topic, PJ::Timestamp{sample.ts_ns}, PJ::Span<const PJ::sdk::NamedFieldValue>(&field, 1));
    if (!status) {
      first_error_ = status.error();
    }
  }

  const std::string& error() const {
    return first_error_;
  }

 private:
  const PJ::sdk::SourceWriteHostView& host_;
  std::unordered_map<std::string, PJ::sdk::TopicHandle> topics_;
  std::string first_error_;
};

// File data-source plugin: loads a ros2_tracing LTTng/CTF trace directory and
// ingests derived timing metrics as PlotJuggler series.
class TraceFileSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["filepath"] = filepath_;
    return cfg.dump();
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
      return PJ::unexpected(std::string("no trace path configured"));
    }

    ros2_trace_model::FsTraceSource source(resolveTraceDir(filepath_));
    if (!source.ok()) {
      return PJ::unexpected(source.error());
    }

    WriteHostSink sink(writeHost());
    ros2_trace_model::Pipeline pipeline(sink);
    pipeline.run(source);

    if (!sink.error().empty()) {
      return PJ::unexpected(sink.error());
    }

    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo, "Imported " + std::to_string(source.size()) + " ROS 2 trace events");
    return PJ::okStatus();
  }

 private:
  std::string filepath_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(TraceFileSource, kTraceManifest)
