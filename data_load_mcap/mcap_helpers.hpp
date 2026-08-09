#pragma once

// Note: MCAP_IMPLEMENTATION must be defined in exactly one translation unit
// before including this header.
#include <algorithm>
#include <cstdint>
#include <mcap/reader.hpp>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace PJ::McapHelpers {

/// True when the recording cannot be replayed in log-time order.
///
/// LogTimeOrder needs Message Index records, which are a *separate* thing from
/// Chunk Index records: a chunked recording can carry chunk indexes while
/// having no message indexes at all. That happens in two unrelated situations,
/// neither of which is detectable by asking whether chunkIndexes() is empty:
///
///   * the file was written with McapWriterOptions::noMessageIndex = true —
///     valid, spec-conformant, and produced by real recorders;
///   * the summary section was unusable (damaged footer, writer killed
///     mid-write) so readSummary() fell back to a sequential scan, which
///     synthesizes chunk indexes with messageIndexLength == 0 because a scan
///     cannot recover message indexes.
///
/// Requesting LogTimeOrder in either case makes the reader fail with
/// NoMessageIndexesAvailable and deliver zero messages, so callers must fall
/// back to FileOrder. Mirrors the reader's own guard so the two cannot drift.
///
/// all_of, NOT any_of — a mixed file is not a reason to give up time order.
/// A chunk can legitimately carry messageIndexLength == 0 while its neighbours
/// do not: McapWriter emits the Schema and Channel records for a new channel
/// into the CURRENT chunk and only then checks for overflow, so the chunk it
/// flushes at that point can hold nothing but those records. Such a chunk has
/// no messages, so the parallel planner skipping it (it keys off
/// messageIndexOffsets) loses nothing. Switching to any_of would drop those
/// files to serial FileOrder for no benefit. The genuinely lossy shape —
/// message-bearing chunks with no indexes alongside indexed ones — needs
/// noMessageIndex to vary per chunk, which McapWriter cannot do: options_ is
/// assigned once in open() and never mutated.
inline bool lacksMessageIndexes(const std::vector<mcap::ChunkIndex>& chunk_indexes) {
  return chunk_indexes.empty() || std::all_of(
                                      chunk_indexes.begin(), chunk_indexes.end(),
                                      [](const mcap::ChunkIndex& ci) { return ci.messageIndexLength == 0; });
}

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
  auto status = mcap::McapReader::ReadFooter(reader, file_size - mcap::internal::FooterLength, &footer);
  if (!status.ok()) {
    return status;
  }

  if (footer.summaryStart == 0) {
    return mcap::Status{mcap::StatusCode::MissingStatistics, "no summary section"};
  }
  info.summary_start = footer.summaryStart;

  const mcap::ByteOffset summary_offset_start =
      footer.summaryOffsetStart != 0 ? footer.summaryOffsetStart : file_size - mcap::internal::FooterLength;

  if (summary_offset_start <= footer.summaryStart) {
    return mcap::Status{mcap::StatusCode::InvalidFooter, "no SummaryOffset section available"};
  }

  struct GroupRange {
    mcap::ByteOffset start = 0;
    mcap::ByteOffset end = 0;
  };
  GroupRange schema_range, channel_range, stats_range;
  bool found_any = false;

  mcap::RecordReader offset_reader(reader, summary_offset_start, file_size - mcap::internal::FooterLength);
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
    return mcap::Status{mcap::StatusCode::MissingStatistics, "no relevant SummaryOffset records found"};
  }

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
        break;
      }
    }
  }

  if (!info.statistics) {
    return mcap::Status{mcap::StatusCode::MissingStatistics, "Statistics record not found in summary"};
  }
  return mcap::StatusCode::Success;
}

inline void populateSummaryFromReader(const mcap::McapReader& reader, McapSummaryInfo& info) {
  for (const auto& [id, ptr] : reader.schemas()) {
    info.schemas.insert({id, ptr});
  }
  for (const auto& [id, ptr] : reader.channels()) {
    info.channels.insert({id, ptr});
  }
  info.statistics = reader.statistics();
}

}  // namespace PJ::McapHelpers
