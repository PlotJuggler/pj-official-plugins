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

namespace {

using McapSummaryInfo = PJ::McapHelpers::McapSummaryInfo;
using PJ::McapHelpers::populateSummaryFromReader;
using PJ::McapHelpers::readSelectiveSummary;

// ─────────────────────────────────────────────────────────────────────────────
// McapSource plugin
// ─────────────────────────────────────────────────────────────────────────────

class McapSource : public PJ::FileSourceBase {
 public:
  void* dialogContext() override { return &dialog_; }

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

      auto handle = runtimeHost().ensureParserBinding(request);
      if (handle) {
        bindings.emplace(channel_id, *handle);
      } else {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            std::string("no parser for channel '") + channel_ptr->topic +
                "' (encoding: " + std::string(encoding) + "): " + handle.error());
      }
    }

    if (bindings.empty()) {
      reader.close();
      return PJ::unexpected(std::string("no channels could be bound to parsers"));
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
