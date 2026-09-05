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

/// The MCAP spec's "no schema" sentinel. Schema ids are 1-based, so 0 does not
/// mean "broken reference": it means the payload is self-describing and the
/// parser is chosen from the channel's message encoding alone (schemaless JSON
/// is the common case). Shared by the dialog's channel filter and the source's
/// binding loop so the two cannot disagree about what is importable.
inline bool isSchemaless(mcap::SchemaId schema_id) {
  return schema_id == 0;
}

/// True when a reader problem actually ends the scan.
///
/// LinearMessageView::onMessage reports InvalidChannelId / InvalidSchemaId per
/// message and then keeps iterating, so one stray record referencing an
/// unknown channel must not be reported as "the file was only partially
/// recovered". Everything else — decompression failures, truncated records,
/// parse errors — stops the read for good.
inline bool problemIsTerminal(mcap::StatusCode code) {
  return code != mcap::StatusCode::InvalidChannelId && code != mcap::StatusCode::InvalidSchemaId;
}

/// How a finished import run should be reported.
enum class ImportOutcome {
  kSuccess,  ///< everything offered was accepted, or nothing was offered
  kPartial,  ///< the read stopped early but messages landed — keep them, say so
  kFailed,   ///< nothing usable landed; must not be reported as a successful load
};

/// Decide how to report an import run that has finished.
///
/// Closes two silent-success traps, both of which this plugin has shipped:
///   * a message view that fails to initialise reports through status() and
///     then compares begin() == end(), so the push loop never runs and the
///     import would otherwise return ok after loading nothing;
///   * a run that ends CLEANLY while every push was rejected, which stays
///     under the consecutive-failure threshold on any file below that many
///     messages and would likewise return ok with an empty dataset.
///
/// Keyed on messages actually pushed, never on Statistics: a recording can
/// carry an absent, empty, or simply untruthful Statistics record, and whether
/// a read succeeded is not something file metadata gets a vote on.
///
/// @param view_status_ok   the message view's final status
/// @param pushed           messages handed to the host (accepted or rejected)
/// @param accepted         messages the host took
/// @param stop_requested   the user cancelled the import
inline ImportOutcome classifyImportOutcome(
    bool view_status_ok, uint64_t pushed, uint64_t accepted, bool stop_requested) {
  // A cancel is neither a failure nor a partial recovery worth warning about:
  // the truncation was asked for, and whatever landed before it is valid.
  if (stop_requested) {
    return ImportOutcome::kSuccess;
  }
  if (accepted > 0) {
    return view_status_ok ? ImportOutcome::kSuccess : ImportOutcome::kPartial;
  }
  // Nothing accepted. Offering nothing at all is legitimate — the selected
  // topics genuinely held no messages. Offering some and keeping none is not.
  return (!view_status_ok || pushed > 0) ? ImportOutcome::kFailed : ImportOutcome::kSuccess;
}

/// True when a failed indexed read is worth retrying as a serial file-order
/// scan.
///
/// A summary can advertise Message Index records that turn out to be
/// unreadable — truncated, or corrupt after an interrupted write. The indexed
/// reader then dies having produced nothing, even though a file-order scan,
/// which never consults message indexes at all, could still recover every
/// message in the file. lacksMessageIndexes() cannot predict this: it only
/// sees what the summary CLAIMS, not whether the index records parse.
///
/// Retrying is safe only when the first attempt delivered nothing at all.
/// An accepted message would be re-ingested as a duplicate on the second pass,
/// and a merely rejected one means the host — not the reader — is refusing the
/// data, which a different read order will not fix. A cancelled import is
/// never retried.
inline bool shouldRetryInFileOrder(bool view_status_ok, uint64_t pushed, uint64_t accepted, bool stop_requested) {
  return !view_status_ok && !stop_requested && pushed == 0 && accepted == 0;
}

/// Denominator for the import progress bar.
///
/// Prefers the per-channel counts of the selected topics: the file-wide total
/// would strand the bar well short of full whenever the selection is a subset,
/// which is the common case. channelMessageCounts is optional in a Statistics
/// record though — some writers leave it empty, and so does the fallback scan
/// for channels it cannot attribute — and summing it then yields 0, which
/// freezes the bar at zero for the whole import. Fall back to the file-wide
/// count there: too large for a subset selection, but monotonic and bounded,
/// which 0 is not.
inline uint64_t progressTotal(const mcap::Statistics& stats, const std::vector<mcap::ChannelId>& selected_channels) {
  uint64_t total = 0;
  size_t counted = 0;
  for (const mcap::ChannelId id : selected_channels) {
    if (auto it = stats.channelMessageCounts.find(id); it != stats.channelMessageCounts.end()) {
      total += it->second;
      ++counted;
    }
  }
  if (counted == 0 && !selected_channels.empty()) {
    return stats.messageCount;
  }
  return total;
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
