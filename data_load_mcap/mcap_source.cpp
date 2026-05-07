#include <pj_base/sdk/data_source_patterns.hpp>

#define MCAP_IMPLEMENTATION
#include "mcap_dialog.hpp"
#include "mcap_helpers.hpp"
#include "mcap_manifest.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


namespace {

using McapSummaryInfo = PJ::McapHelpers::McapSummaryInfo;
using PJ::McapHelpers::populateSummaryFromReader;
using PJ::McapHelpers::readSelectiveSummary;

// Schemas whose raw payload is worth retaining in the ObjectStore for replay
// or out-of-band rendering (image / video). Match exact `schema->name`. The
// list is intentionally small; extend as new media-bearing message types are
// validated. A future config option in `McapDialog` may let the user override
// this set per-load.
const std::unordered_set<std::string>& blobBearingSchemas() {
  static const std::unordered_set<std::string> kSet = {
      "sensor_msgs/Image", "sensor_msgs/msg/Image"
  };
  return kSet;
}

// ─────────────────────────────────────────────────────────────────────────────
// McapSource plugin
// ─────────────────────────────────────────────────────────────────────────────

class McapSource : public PJ::FileSourceBase {
 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override { return dialog_.saveConfig(); }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    if (dialog_.filepath().empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }

    mcap::McapReader reader;
    auto status = reader.open(dialog_.filepath());
    if (!status.ok()) {
      return PJ::unexpected(std::string("cannot open MCAP file: ") + status.message);
    }

    // --- Read summary (schemas, channels, statistics) ---
    McapSummaryInfo summary;
    bool used_selective_summary = false;
    status = readSelectiveSummary(*reader.dataSource(), summary);
    if (status.ok()) {
      used_selective_summary = true;
    } else {
      status = reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
      if (!status.ok()) {
        runtimeHost().showError("Can't open summary of the file",
                                "Code: " + std::to_string(static_cast<int>(status.code)) +
                                    "\nMessage: " + status.message);
        reader.close();
        return PJ::unexpected(std::string("cannot read MCAP summary: ") + status.message);
      }
      populateSummaryFromReader(reader, summary);
    }

    uint64_t total_messages = 0;
    if (summary.statistics) {
      total_messages = summary.statistics->messageCount;
    }
    (void)runtimeHost().progressStart("Importing MCAP", total_messages, true);

    // --- Build parser config JSON from dialog settings ---
    nlohmann::json parser_config;
    parser_config["max_array_size"] = dialog_.maxArraySize();
    parser_config["use_embedded_timestamp"] = dialog_.useTimestamp();
    parser_config["clamp_large_arrays"] = dialog_.clampLargeArrays();
    std::string parser_config_str = parser_config.dump();

    // --- Ensure parser bindings for selected channels ---
    const auto& selected = dialog_.selectedTopics();
    std::unordered_map<mcap::ChannelId, PJ::ParserBindingHandle> bindings;
    // Per-channel object topics for blob-bearing schemas. Populated only when
    // the host registered `pj.source_object_write.v1` AND the channel's
    // schema is in the blob-bearing set. Empty otherwise — the message-loop
    // below skips the lazy push when a channel isn't present.
    std::unordered_map<mcap::ChannelId, PJ::sdk::ObjectTopicHandle> object_topics;
    std::vector<std::string> binding_errors;

    for (const auto& [channel_id, channel_ptr] : summary.channels) {
      // Filter by dialog selection
      if (selected.find(channel_ptr->topic) == selected.end()) {
        continue;
      }

      auto schema_it = summary.schemas.find(channel_ptr->schemaId);
      if (schema_it == summary.schemas.end()) continue;
      const auto& schema = schema_it->second;

      PJ::Span<const uint8_t> schema_bytes{
          reinterpret_cast<const uint8_t*>(schema->data.data()),
          schema->data.size()};

      std::string_view encoding = channel_ptr->messageEncoding;
      if (encoding.empty()) encoding = schema->encoding;

      PJ::ParserBindingRequest request{
          .topic_name = channel_ptr->topic,
          .parser_encoding = encoding,
          .type_name = schema->name,
          .schema = schema_bytes,
          .parser_config_json = parser_config_str,
      };

      // Register the parallel object topic BEFORE binding the parser. The
      // runtime host resolves the parser-side write surface against an
      // already-registered topic (via ObjectStore::findTopic), so the source
      // must own and complete its source-side registration first. Failure is
      // non-fatal — the scalar ingest path continues; the warning surfaces
      // via reportMessage.
      if (objectWriteHost() != nullptr && blobBearingSchemas().count(schema->name) != 0) {
        auto obj_topic = objectWriteHost()->registerTopic(channel_ptr->topic, /*metadata_json*/ "{}");
        if (obj_topic) {
          object_topics.emplace(channel_id, *obj_topic);
        } else {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning,
              std::string("failed to register object topic for '") + channel_ptr->topic +
                  "': " + obj_topic.error());
        }
      }

      auto handle = runtimeHost().ensureParserBinding(request);
      if (handle) {
        bindings.emplace(channel_id, *handle);
      } else {
        binding_errors.push_back(
            channel_ptr->topic + " (encoding: " + std::string(encoding) + "): " + handle.error());
        continue;
      }
    }

    if (bindings.empty()) {
      std::string msg = "No channels could be bound to parsers:\n";
      for (const auto& e : binding_errors) {
        msg += "  - " + e + "\n";
      }
      runtimeHost().showError("Parser Error", msg);
      reader.close();
      return PJ::unexpected(msg);
    }

    if (!binding_errors.empty()) {
      std::string msg = std::to_string(binding_errors.size()) + " channel(s) skipped (no parser):\n";
      for (const auto& e : binding_errors) {
        msg += "  - " + e + "\n";
      }
      runtimeHost().showWarning("Parser Error", msg);
    }

    // --- Iterate messages and push raw bytes ---
    auto on_problem = [this](const mcap::Status& problem) {
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, problem.message);
    };

    auto create_message_view = [&]() -> mcap::LinearMessageView {
      if (used_selective_summary) {
        auto [data_start, data_end_unused] = reader.byteRange(0);
        (void)data_end_unused;
        return mcap::LinearMessageView(reader, data_start, summary.summary_start, 0,
                                       mcap::MaxTime, on_problem);
      }
      return reader.readMessages(on_problem);
    };

    auto messages = create_message_view();
    uint64_t msg_count = 0;
    bool use_log_time = dialog_.useMcapLogTime();

    for (const auto& msg_view : messages) {
      auto binding_it = bindings.find(msg_view.channel->id);
      if (binding_it == bindings.end()) continue;

      // Select timestamp based on dialog setting
      PJ::Timestamp timestamp_ns = static_cast<PJ::Timestamp>(
          use_log_time ? msg_view.message.logTime : msg_view.message.publishTime);

      PJ::Span<const uint8_t> payload{
          reinterpret_cast<const uint8_t*>(msg_view.message.data),
          msg_view.message.dataSize};

      auto push_status = runtimeHost().pushRawMessage(binding_it->second, timestamp_ns, payload);
      if (!push_status) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            std::string("push failed on '") + msg_view.channel->topic +
                "': " + push_status.error());
      }

      // Lazy retention of the raw message in ObjectStore for blob-bearing
      // channels. Capture (file_path, channel_id, log_time): the closure
      // reopens the file and reads the message at that coordinate when (and
      // only when) something downstream resolves the entry. No bytes are
      // copied here at ingest time.
      auto obj_it = object_topics.find(msg_view.channel->id);
      if (obj_it != object_topics.end()) {
        const auto obj_handle = obj_it->second;
        const auto channel_id_capture = msg_view.channel->id;
        const auto log_time_capture = msg_view.message.logTime;
        std::string file_path_capture = dialog_.filepath();
        auto lazy_status = objectWriteHost()->pushLazy(
            obj_handle, timestamp_ns,
            [path = std::move(file_path_capture), channel_id_capture, log_time_capture]() -> std::vector<uint8_t> {
              mcap::McapReader r;
              if (!r.open(path).ok()) return {};
              auto noop_problem = [](const mcap::Status&) {};
              for (const auto& v : r.readMessages(noop_problem, log_time_capture, log_time_capture + 1)) {
                if (v.channel->id == channel_id_capture) {
                  return std::vector<uint8_t>(
                      reinterpret_cast<const uint8_t*>(v.message.data),
                      reinterpret_cast<const uint8_t*>(v.message.data) + v.message.dataSize);
                }
              }
              return {};
            });
        if (!lazy_status) {
          runtimeHost().reportMessage(
              PJ::DataSourceMessageLevel::kWarning,
              std::string("object pushLazy failed on '") + msg_view.channel->topic +
                  "': " + lazy_status.error());
        }
      }

      ++msg_count;
      if (msg_count % 1000 == 0) {
        if (!runtimeHost().progressUpdate(msg_count)) break;
        if (runtimeHost().isStopRequested()) break;
      }
    }

    reader.close();
    return PJ::okStatus();
  }

 private:
  McapDialog dialog_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(McapSource, kMcapManifest)

PJ_DIALOG_PLUGIN(McapDialog)
