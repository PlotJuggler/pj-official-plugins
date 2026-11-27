#pragma once

// MCAP_IMPLEMENTATION must be defined in exactly one translation unit before including this header.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mcap/reader.hpp>
#include <memory>
#include <optional>
#include <pj_utf8_path/utf8_path.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace PJ::McapHelpers {

/// True when the recording cannot be replayed in log-time order.
///
/// LogTimeOrder needs Message Index records, which are separate from Chunk Index records: a chunked recording can
/// carry chunk indexes and no message indexes, and chunkIndexes().empty() detects neither case:
///   * the file was written with McapWriterOptions::noMessageIndex = true (valid, produced by real recorders);
///   * the summary was unusable (damaged footer, writer killed mid-write), so readSummary() fell back to a sequential
///     scan, which synthesizes chunk indexes with messageIndexLength == 0 (a scan cannot recover message indexes).
/// LogTimeOrder then fails with NoMessageIndexesAvailable and delivers zero messages, so callers must fall back to
/// FileOrder. Mirrors the reader's own guard.
///
/// all_of, NOT any_of: a chunk can legitimately have messageIndexLength == 0 while its neighbours do not. McapWriter
/// emits the Schema and Channel records of a new channel into the CURRENT chunk before checking for overflow, so the
/// chunk it then flushes can hold nothing but those records; it has no messages, so the parallel planner skipping it
/// loses nothing, while any_of would drop such files to serial FileOrder. The genuinely lossy shape (message-bearing
/// chunks without indexes next to indexed ones) needs noMessageIndex to vary per chunk, which McapWriter cannot do.
inline bool lacksMessageIndexes(const std::vector<mcap::ChunkIndex>& chunk_indexes) {
  return chunk_indexes.empty() || std::all_of(
                                      chunk_indexes.begin(), chunk_indexes.end(),
                                      [](const mcap::ChunkIndex& ci) { return ci.messageIndexLength == 0; });
}

/// The MCAP spec's "no schema" sentinel. Schema ids are 1-based, so 0 is not a broken reference: the payload is
/// self-describing and the parser is chosen from the channel's message encoding alone (schemaless JSON is the common
/// case). Shared by the dialog's channel filter and the source's binding loop so they agree on what is importable.
inline bool isSchemaless(mcap::SchemaId schema_id) {
  return schema_id == 0;
}

/// True when a reader problem actually ends the scan. LinearMessageView::onMessage reports InvalidChannelId /
/// InvalidSchemaId per message and keeps iterating, so one stray record with an unknown channel must not be reported as
/// "the file was only partially recovered". Everything else (decompression failures, truncated records, parse errors)
/// stops the read for good.
inline bool problemIsTerminal(mcap::StatusCode code) {
  return code != mcap::StatusCode::InvalidChannelId && code != mcap::StatusCode::InvalidSchemaId;
}

/// How a finished import run should be reported.
enum class ImportOutcome {
  kSuccess,  ///< everything offered was accepted, or nothing was offered
  kPartial,  ///< the read stopped early but messages landed — keep them, say so
  kFailed,   ///< nothing usable landed; must not be reported as a successful load
};

/// How to report a finished import run. Closes two silent-success traps:
///   * a message view that fails to initialise reports through status() and then compares begin() == end(), so the
///     push loop never runs and the import would return ok after loading nothing;
///   * a run that ends CLEANLY while every push was rejected, which stays under the consecutive-failure threshold on
///     any file below that many messages and would likewise return ok with an empty dataset.
///
/// Keyed on messages actually pushed, never on Statistics: a recording can carry an absent, empty or untruthful
/// Statistics record, and file metadata gets no vote on whether a read succeeded.
///
/// `pushed` counts messages handed to the host (accepted or rejected), `accepted` those the host took.
inline ImportOutcome classifyImportOutcome(
    bool view_status_ok, uint64_t pushed, uint64_t accepted, bool stop_requested) {
  // A cancel is neither a failure nor a partial recovery worth warning about: the truncation was asked for.
  if (stop_requested) {
    return ImportOutcome::kSuccess;
  }
  if (accepted > 0) {
    return view_status_ok ? ImportOutcome::kSuccess : ImportOutcome::kPartial;
  }
  // Nothing accepted. Offering nothing is legitimate (the selected topics held no messages); offering some and keeping
  // none is not.
  return (!view_status_ok || pushed > 0) ? ImportOutcome::kFailed : ImportOutcome::kSuccess;
}

/// True when a failed indexed read is worth retrying as a serial file-order scan.
///
/// A summary can advertise Message Index records that turn out to be unreadable (truncated, or corrupt after an
/// interrupted write): the indexed reader then dies having produced nothing, while a file-order scan, which never
/// consults message indexes, could still recover every message. lacksMessageIndexes() cannot predict this: it only
/// sees what the summary CLAIMS.
///
/// Safe only when the first attempt delivered nothing at all: an accepted message would be re-ingested as a duplicate,
/// and a merely rejected one means the host is refusing the data, which another read order will not fix. A cancelled
/// import is never retried.
inline bool shouldRetryInFileOrder(bool view_status_ok, uint64_t pushed, uint64_t accepted, bool stop_requested) {
  return !view_status_ok && !stop_requested && pushed == 0 && accepted == 0;
}

/// Denominator for the import progress bar. Prefers the per-channel counts of the selected topics: the file-wide total
/// would strand the bar short of full for a subset selection. channelMessageCounts is optional in a Statistics record
/// (some writers leave it empty, and so does the fallback scan for channels it cannot attribute), and summing it then
/// gives 0, which freezes the bar at zero. Fall back to the file-wide count there: too large for a subset, but
/// monotonic and bounded, which 0 is not.
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

struct McapSummaryInfo {
  std::unordered_map<mcap::SchemaId, mcap::SchemaPtr> schemas;
  std::unordered_map<mcap::ChannelId, mcap::ChannelPtr> channels;
  std::optional<mcap::Statistics> statistics;
  mcap::ByteOffset summary_start = 0;
};

/// Read only Schema, Channel, and Statistics records from the MCAP summary
/// by using SummaryOffset entries to seek directly to each group, skipping
/// expensive MessageIndex and ChunkIndex data.
///
/// Succeeds only when the three groups read back whole: every record parses and has its group's opcode,
/// and no fewer channels and schemas were found than Statistics counts. Anything else (a SummaryOffset
/// that points at the wrong place, a record the writer damaged) is an error, so the caller falls back to
/// McapReader::readSummary, which does not trust the offsets. The Statistics counts are a floor only:
/// some writers count just the channels that hold messages, so a summary may list more.
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

  // A writer may describe one group with several SummaryOffset records: keep every range.
  using GroupRanges = std::vector<std::pair<mcap::ByteOffset, mcap::ByteOffset>>;
  GroupRanges schema_ranges, channel_ranges, stats_ranges;

  mcap::RecordReader offset_reader(reader, summary_offset_start, file_size - mcap::internal::FooterLength);
  while (auto record = offset_reader.next()) {
    if (record->opcode != mcap::OpCode::SummaryOffset) {
      continue;
    }
    mcap::SummaryOffset so;
    if (status = mcap::McapReader::ParseSummaryOffset(*record, &so); !status.ok()) {
      return status;
    }
    const std::pair<mcap::ByteOffset, mcap::ByteOffset> range{so.groupStart, so.groupStart + so.groupLength};
    if (so.groupOpCode == mcap::OpCode::Schema) {
      schema_ranges.push_back(range);
    } else if (so.groupOpCode == mcap::OpCode::Channel) {
      channel_ranges.push_back(range);
    } else if (so.groupOpCode == mcap::OpCode::Statistics) {
      stats_ranges.push_back(range);
    }
  }
  if (!offset_reader.status().ok()) {
    return offset_reader.status();
  }

  if (schema_ranges.empty() && channel_ranges.empty() && stats_ranges.empty()) {
    return mcap::Status{mcap::StatusCode::MissingStatistics, "no relevant SummaryOffset records found"};
  }

  const auto read_group = [&reader](const GroupRanges& ranges, mcap::OpCode opcode, const auto& parse) -> mcap::Status {
    for (const auto& [start, end] : ranges) {
      mcap::RecordReader records(reader, start, end);
      while (auto record = records.next()) {
        if (record->opcode != opcode) {
          return mcap::Status{mcap::StatusCode::InvalidRecord, "unexpected record in a summary group"};
        }
        if (auto parsed = parse(*record); !parsed.ok()) {
          return parsed;
        }
      }
      if (!records.status().ok()) {
        return records.status();
      }
    }
    return mcap::Status{mcap::StatusCode::Success};
  };

  status = read_group(schema_ranges, mcap::OpCode::Schema, [&info](const mcap::Record& record) {
    auto ptr = std::make_shared<mcap::Schema>();
    auto parsed = mcap::McapReader::ParseSchema(record, ptr.get());
    if (parsed.ok()) {
      info.schemas.try_emplace(ptr->id, ptr);
    }
    return parsed;
  });
  if (status.ok()) {
    status = read_group(channel_ranges, mcap::OpCode::Channel, [&info](const mcap::Record& record) {
      auto ptr = std::make_shared<mcap::Channel>();
      auto parsed = mcap::McapReader::ParseChannel(record, ptr.get());
      if (parsed.ok()) {
        info.channels.try_emplace(ptr->id, ptr);
      }
      return parsed;
    });
  }
  if (status.ok()) {
    status = read_group(stats_ranges, mcap::OpCode::Statistics, [&info](const mcap::Record& record) {
      mcap::Statistics stats;
      auto parsed = mcap::McapReader::ParseStatistics(record, &stats);
      if (parsed.ok()) {
        info.statistics = std::move(stats);
      }
      return parsed;
    });
  }
  if (!status.ok()) {
    return status;
  }

  if (!info.statistics) {
    return mcap::Status{mcap::StatusCode::MissingStatistics, "Statistics record not found in summary"};
  }
  if (info.schemas.size() < info.statistics->schemaCount || info.channels.size() < info.statistics->channelCount) {
    return mcap::Status{mcap::StatusCode::InvalidRecord, "summary groups hold fewer records than Statistics counts"};
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

/// Schemas, channels and statistics of an opened file: all the import dialog needs, on the GUI thread.
/// readSummary() parses every ChunkIndex and builds an interval tree over them, about half a second for
/// a bag with 300k chunks, so the SummaryOffset records are tried first. The full read, with its fallback
/// scan, runs whenever readSelectiveSummary() fails: no summary, no SummaryOffset section, no Statistics,
/// or a summary whose groups are damaged or incomplete.
inline mcap::Status readChannelSummary(mcap::McapReader& reader, McapSummaryInfo& info) {
  info = {};
  if (mcap::IReadable* source = reader.dataSource(); source != nullptr && readSelectiveSummary(*source, info).ok()) {
    return mcap::StatusCode::Success;
  }

  info = {};
  const auto status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (status.ok()) {
    populateSummaryFromReader(reader, info);
  }
  return status;
}

/// What a rosbag2 metadata.yaml says about where its data lives.
struct Rosbag2Metadata {
  bool is_rosbag2 = false;         ///< top-level rosbag2_bagfile_information key present
  std::string storage_identifier;  ///< "mcap", "sqlite3", ... ("" when absent)
  std::vector<std::string> relative_file_paths;
  std::optional<std::string> compression_format;  ///< "zstd", ... (nullopt when absent)
  std::optional<std::string> compression_mode;    ///< as written: "MESSAGE", "FILE", "none", "" (nullopt when absent)
};

/// Line scan of the rosbag2 metadata keys this plugin needs, in either form rosbag2 writes them: metadata.yaml, under
/// a top-level `rosbag2_bagfile_information:` key, or the bare body it embeds in each MCAP file's `rosbag2` Metadata
/// record. Not a YAML parser: it relies on the block style rosbag2's yaml-cpp emitter always writes (one `- item` per
/// line under `relative_file_paths:`).
inline Rosbag2Metadata parseRosbag2Metadata(std::string_view yaml) {
  const auto trim = [](std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
      return std::string_view{};
    }
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
  };
  const auto unquote = [](std::string_view s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
      s = s.substr(1, s.size() - 2);
    }
    return std::string(s);
  };

  Rosbag2Metadata out;
  bool in_file_list = false;
  // The compression keys count only at the top level of the bag information (column 0 in the bare body, one level
  // under the wrapper key), so a same-named key nested under `files:` or a topic cannot decide how payloads are read.
  size_t top_level_indent = 0;
  bool top_level_indent_pending = false;
  std::istringstream lines{std::string(yaml)};
  for (std::string raw; std::getline(lines, raw);) {
    const std::string_view line = trim(raw);
    const size_t indent = raw.find_first_not_of(' ');
    if (top_level_indent_pending && !line.empty()) {
      top_level_indent = indent;
      top_level_indent_pending = false;
    }
    const bool top_level = indent == top_level_indent;
    const auto value_of = [&](std::string_view key) { return unquote(trim(line.substr(key.size()))); };
    if (in_file_list) {
      if (line.starts_with("- ")) {
        out.relative_file_paths.push_back(unquote(trim(line.substr(2))));
        continue;
      }
      in_file_list = false;
    }
    if (line == "rosbag2_bagfile_information:") {
      out.is_rosbag2 = true;
      top_level_indent_pending = true;
    } else if (line.starts_with("storage_identifier:")) {
      out.storage_identifier = value_of("storage_identifier:");
    } else if (line == "relative_file_paths:") {
      in_file_list = true;
    } else if (top_level && line.starts_with("compression_format:")) {
      out.compression_format = value_of("compression_format:");
    } else if (top_level && line.starts_with("compression_mode:")) {
      out.compression_mode = value_of("compression_mode:");
    }
  }
  return out;
}

/// The MCAP files making up the recording at `path`, or an error message.
struct RecordingFiles {
  std::vector<std::string> paths;
  std::string error;
};

/// A plain .mcap resolves to itself. A rosbag2 metadata.yaml resolves to the
/// split files it lists, in recording order, relative to the yaml's folder.
/// Only MCAP-storage bags are accepted: sqlite3 (.db3) bags and bags split
/// with per-file compression (.mcap.zstd) are rejected, never half-loaded.
inline RecordingFiles resolveRecordingFiles(const std::string& path) {
  namespace fs = std::filesystem;
  // A string test on the UTF-8 name: building a filesystem path from it would read it in the ANSI code page on
  // Windows, and path::string() would give back ANSI bytes. `ext` is lower case.
  const auto has_extension = [](std::string_view name, std::string_view ext) {
    return name.size() >= ext.size() &&
           std::equal(
               name.end() - static_cast<std::ptrdiff_t>(ext.size()), name.end(), ext.begin(),
               [](char a, char b) { return ((a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a) == b; });
  };

  if (!has_extension(path, ".yaml") && !has_extension(path, ".yml")) {
    return {{path}, {}};
  }

  const fs::path yaml_path = utf8_path::pathFromUtf8(path);
  std::ifstream in(yaml_path, std::ios::binary);
  if (!in) {
    return {{}, "Cannot open " + path};
  }
  std::stringstream content;
  content << in.rdbuf();
  const Rosbag2Metadata meta = parseRosbag2Metadata(content.str());

  if (!meta.is_rosbag2) {
    return {{}, "Not a ROS 2 bag metadata.yaml: " + path};
  }
  if (!meta.storage_identifier.empty() && meta.storage_identifier != "mcap") {
    return {
        {},
        "Only MCAP-storage ROS 2 bags are supported; this bag uses '" + meta.storage_identifier +
            "'. Convert it with 'ros2 bag convert' (storage_id: mcap)."};
  }
  if (meta.relative_file_paths.empty()) {
    return {{}, "The ROS 2 bag metadata lists no data files: " + path};
  }

  RecordingFiles out;
  for (const std::string& relative : meta.relative_file_paths) {
    const fs::path file = yaml_path.parent_path() / utf8_path::pathFromUtf8(relative);
    if (!has_extension(relative, ".mcap")) {
      return {{}, "Only MCAP-storage ROS 2 bags are supported; unsupported bag file: " + relative};
    }
    if (!fs::exists(file)) {
      return {{}, "ROS 2 bag file listed in metadata.yaml is missing: " + utf8_path::pathToUtf8(file)};
    }
    out.paths.push_back(utf8_path::pathToUtf8(file));
  }
  return out;
}

}  // namespace PJ::McapHelpers
