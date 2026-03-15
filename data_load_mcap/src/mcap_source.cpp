#include <pj_base/sdk/data_source_patterns.hpp>

#define MCAP_IMPLEMENTATION
#include <mcap/reader.hpp>

#include "mcap_dialog.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

/// Summary data extracted from the MCAP file footer/summary section.
struct McapSummaryInfo {
  std::unordered_map<mcap::SchemaId, mcap::SchemaPtr> schemas;
  std::unordered_map<mcap::ChannelId, mcap::ChannelPtr> channels;
  std::optional<mcap::Statistics> statistics;
  mcap::ByteOffset summary_start = 0;
};

/// Read only Schema, Channel, and Statistics records from the MCAP summary
/// by using SummaryOffset entries to seek directly to each group, skipping
/// expensive MessageIndex and ChunkIndex data.
mcap::Status readSelectiveSummary(mcap::IReadable& reader, McapSummaryInfo& info) {
  const uint64_t file_size = reader.size();

  mcap::Footer footer;
  auto status =
      mcap::McapReader::ReadFooter(reader, file_size - mcap::internal::FooterLength, &footer);
  if (!status.ok()) return status;

  if (footer.summaryStart == 0) {
    return mcap::Status{mcap::StatusCode::MissingStatistics, "no summary section"};
  }
  info.summary_start = footer.summaryStart;

  const mcap::ByteOffset summary_offset_start =
      footer.summaryOffsetStart != 0 ? footer.summaryOffsetStart
                                     : file_size - mcap::internal::FooterLength;

  if (summary_offset_start <= footer.summaryStart) {
    return mcap::Status{mcap::StatusCode::InvalidFooter, "no SummaryOffset section available"};
  }

  struct GroupRange {
    mcap::ByteOffset start = 0;
    mcap::ByteOffset end = 0;
  };
  GroupRange schema_range, channel_range, stats_range;
  bool found_any = false;

  mcap::RecordReader offset_reader(reader, summary_offset_start,
                                   file_size - mcap::internal::FooterLength);
  while (auto record = offset_reader.next()) {
    if (record->opcode != mcap::OpCode::SummaryOffset) continue;
    mcap::SummaryOffset so;
    if (!mcap::McapReader::ParseSummaryOffset(*record, &so).ok()) continue;
    if (so.groupOpCode == mcap::OpCode::Schema) {
      schema_range = {so.groupStart, so.groupStart + so.groupLength};
      found_any = true;
    } else if (so.groupOpCode == mcap::OpCode::Channel) {
      channel_range = {so.groupStart, so.groupStart + so.groupLength};
      found_any = true;
    } else if (so.groupOpCode == mcap::OpCode::Statistics) {
      stats_range = {so.groupStart, so.groupStart + so.groupLength};
      found_any = true;
    }
  }

  if (!found_any) {
    return mcap::Status{mcap::StatusCode::MissingStatistics, "no relevant SummaryOffset records found"};
  }

  if (schema_range.start != 0) {
    mcap::RecordReader rdr(reader, schema_range.start, schema_range.end);
    while (auto record = rdr.next()) {
      if (record->opcode != mcap::OpCode::Schema) continue;
      auto ptr = std::make_shared<mcap::Schema>();
      if (mcap::McapReader::ParseSchema(*record, ptr.get()).ok()) {
        info.schemas.try_emplace(ptr->id, ptr);
      }
    }
  }
  if (channel_range.start != 0) {
    mcap::RecordReader rdr(reader, channel_range.start, channel_range.end);
    while (auto record = rdr.next()) {
      if (record->opcode != mcap::OpCode::Channel) continue;
      auto ptr = std::make_shared<mcap::Channel>();
      if (mcap::McapReader::ParseChannel(*record, ptr.get()).ok()) {
        info.channels.try_emplace(ptr->id, ptr);
      }
    }
  }
  if (stats_range.start != 0) {
    mcap::RecordReader rdr(reader, stats_range.start, stats_range.end);
    while (auto record = rdr.next()) {
      if (record->opcode != mcap::OpCode::Statistics) continue;
      mcap::Statistics stats;
      if (mcap::McapReader::ParseStatistics(*record, &stats).ok()) {
        info.statistics = stats;
        break;
      }
    }
  }

  if (!info.statistics) {
    return mcap::Status{mcap::StatusCode::MissingStatistics, "Statistics record not found in summary"};
  }
  return mcap::StatusCode::Success;
}

void populateSummaryFromReader(const mcap::McapReader& reader, McapSummaryInfo& info) {
  for (const auto& [id, ptr] : reader.schemas()) info.schemas.insert({id, ptr});
  for (const auto& [id, ptr] : reader.channels()) info.channels.insert({id, ptr});
  info.statistics = reader.statistics();
}

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

PJ_DATA_SOURCE_PLUGIN(McapSource,
    R"({"name":"MCAP File Source","version":"1.0.0","file_extensions":[".mcap"]})")

PJ_DIALOG_PLUGIN(McapDialog)
