#pragma once

// Note: MCAP_IMPLEMENTATION must be defined in exactly one translation unit
// before including this header.
#include <mcap/reader.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

namespace PJ::McapHelpers {

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
inline mcap::Status readSelectiveSummary(mcap::IReadable& reader, McapSummaryInfo& info) {
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
    return mcap::Status{mcap::StatusCode::MissingStatistics,
                        "no relevant SummaryOffset records found"};
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
    return mcap::Status{mcap::StatusCode::MissingStatistics,
                        "Statistics record not found in summary"};
  }
  return mcap::StatusCode::Success;
}

inline void populateSummaryFromReader(const mcap::McapReader& reader, McapSummaryInfo& info) {
  for (const auto& [id, ptr] : reader.schemas()) info.schemas.insert({id, ptr});
  for (const auto& [id, ptr] : reader.channels()) info.channels.insert({id, ptr});
  info.statistics = reader.statistics();
}

}  // namespace PJ::McapHelpers
