#include <pj_base/sdk/data_source_patterns.hpp>

#include <mcap/internal.hpp>
#include <mcap/reader.hpp>

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

  // 1. Read the Footer (last 37 bytes of the file).
  mcap::Footer footer;
  auto status =
      mcap::McapReader::ReadFooter(reader, file_size - mcap::internal::FooterLength, &footer);
  if (!status.ok()) {
    return status;
  }

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

  // 2. Read the SummaryOffset section to find group byte ranges.
  struct GroupRange {
    mcap::ByteOffset start = 0;
    mcap::ByteOffset end = 0;
  };
  GroupRange schema_range;
  GroupRange channel_range;
  GroupRange stats_range;
  bool found_any = false;

  mcap::RecordReader offset_reader(reader, summary_offset_start,
                                   file_size - mcap::internal::FooterLength);
  while (auto record = offset_reader.next()) {
    if (record->opcode != mcap::OpCode::SummaryOffset) {
      continue;
    }
    mcap::SummaryOffset so;
    if (!mcap::McapReader::ParseSummaryOffset(*record, &so).ok()) {
      continue;
    }
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
    return mcap::Status{mcap::StatusCode::MissingStatistics,
                        "no relevant SummaryOffset records found"};
  }

  // 3. Read each targeted group.
  if (schema_range.start != 0) {
    mcap::RecordReader rdr(reader, schema_range.start, schema_range.end);
    while (auto record = rdr.next()) {
      if (record->opcode != mcap::OpCode::Schema) {
        continue;
      }
      auto ptr = std::make_shared<mcap::Schema>();
      if (mcap::McapReader::ParseSchema(*record, ptr.get()).ok()) {
        info.schemas.try_emplace(ptr->id, ptr);
      }
    }
  }

  if (channel_range.start != 0) {
    mcap::RecordReader rdr(reader, channel_range.start, channel_range.end);
    while (auto record = rdr.next()) {
      if (record->opcode != mcap::OpCode::Channel) {
        continue;
      }
      auto ptr = std::make_shared<mcap::Channel>();
      if (mcap::McapReader::ParseChannel(*record, ptr.get()).ok()) {
        info.channels.try_emplace(ptr->id, ptr);
      }
    }
  }

  if (stats_range.start != 0) {
    mcap::RecordReader rdr(reader, stats_range.start, stats_range.end);
    while (auto record = rdr.next()) {
      if (record->opcode != mcap::OpCode::Statistics) {
        continue;
      }
      mcap::Statistics stats;
      if (mcap::McapReader::ParseStatistics(*record, &stats).ok()) {
        info.statistics = stats;
        break;  // only one Statistics record expected
      }
    }
  }

  if (!info.statistics) {
    return mcap::Status{mcap::StatusCode::MissingStatistics,
                        "Statistics record not found in summary"};
  }

  return mcap::StatusCode::Success;
}

/// Read summary the standard way (full readSummary) and populate McapSummaryInfo.
void populateSummaryFromReader(const mcap::McapReader& reader, McapSummaryInfo& info) {
  for (const auto& [id, ptr] : reader.schemas()) {
    info.schemas.insert({id, ptr});
  }
  for (const auto& [id, ptr] : reader.channels()) {
    info.channels.insert({id, ptr});
  }
  info.statistics = reader.statistics();
}

// ─────────────────────────────────────────────────────────────────────────────
// McapSource plugin
// ─────────────────────────────────────────────────────────────────────────────

class McapSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override { return PJ::kCapabilityDelegatedIngest; }

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

    mcap::McapReader reader;
    auto status = reader.open(filepath_);
    if (!status.ok()) {
      return PJ::unexpected(
          std::string("cannot open MCAP file: ") + status.message);
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
        return PJ::unexpected(
            std::string("cannot read MCAP summary: ") + status.message);
      }
      populateSummaryFromReader(reader, summary);
    }

    // --- Count total messages for progress reporting ---
    uint64_t total_messages = 0;
    if (summary.statistics) {
      total_messages = summary.statistics->messageCount;
    }

    if (!runtimeHost().progressStart("Importing MCAP", total_messages, true)) {
      // Progress not supported; continue without it.
    }

    // --- Ensure parser bindings for each channel ---
    // Maps channel_id -> binding handle for channels that have a valid parser.
    std::unordered_map<mcap::ChannelId, PJ::ParserBindingHandle> bindings;

    for (const auto& [channel_id, channel_ptr] : summary.channels) {
      auto schema_it = summary.schemas.find(channel_ptr->schemaId);
      if (schema_it == summary.schemas.end()) {
        continue;
      }
      const auto& schema = schema_it->second;

      // Build schema bytes span from the mcap schema data.
      PJ::Span<const uint8_t> schema_bytes{
          reinterpret_cast<const uint8_t*>(schema->data.data()),
          schema->data.size()};

      // Try channel messageEncoding first, then schema encoding.
      std::string_view encoding = channel_ptr->messageEncoding;
      if (encoding.empty()) {
        encoding = schema->encoding;
      }

      PJ::ParserBindingRequest request{
          .topic_name = channel_ptr->topic,
          .parser_encoding = encoding,
          .type_name = schema->name,
          .schema = schema_bytes,
          .parser_config_json = {},
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

    for (const auto& msg_view : messages) {
      auto binding_it = bindings.find(msg_view.channel->id);
      if (binding_it == bindings.end()) {
        continue;
      }

      // MCAP timestamps are in nanoseconds; push as-is (host expects nanoseconds).
      PJ::Timestamp timestamp_ns =
          static_cast<PJ::Timestamp>(msg_view.message.logTime);

      PJ::Span<const uint8_t> payload{
          reinterpret_cast<const uint8_t*>(msg_view.message.data),
          msg_view.message.dataSize};

      auto push_status =
          runtimeHost().pushRawMessage(binding_it->second, timestamp_ns, payload);
      if (!push_status) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            std::string("push failed on '") + msg_view.channel->topic +
                "': " + push_status.error());
      }

      ++msg_count;
      if (msg_count % 1000 == 0) {
        if (!runtimeHost().progressUpdate(msg_count)) {
          // User cancelled.
          break;
        }
        if (runtimeHost().isStopRequested()) {
          break;
        }
      }
    }

    reader.close();
    return PJ::okStatus();
  }

 private:
  std::string filepath_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(McapSource,
    R"({"name":"MCAP File Source","version":"1.0.0","file_extensions":[".mcap"]})")
