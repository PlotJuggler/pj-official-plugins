#pragma once

// The dataset-metadata JSON document of an opened MCAP file: summary facts, every file-level Metadata record (in file
// order, repeated names preserved), plus parsed views of PlotJuggler's own records: `pj.capture` (capture manifest,
// with the source descriptor recovered from its `pj.source.v1` identity framing) and `pj.recording` (recording session
// facts). The host renders the document generically; all PJ-format knowledge lives here.
//
// Extraction is defensive by contract: a malformed or oversized record is skipped with a diagnostic and never fails
// the load.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <mcap/reader.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PJ::McapMetadata {

/// Per-record ceiling on serialized entries: a bigger record is replaced by a stub entry plus a diagnostic, so one
/// rogue record cannot bloat the document.
inline constexpr size_t kMaxRecordBytes = 256 * 1024;

/// Provider id + descriptor bytes recovered from a source-cache identity
/// (`pj.source.v1|<provider-byte-length>|<provider>|<descriptor-json>`).
struct IdentityParts {
  std::string provider_id;
  std::string descriptor_json;
};

/// Returns nullopt for any other framing (unknown version token, bad length, truncated bytes).
inline std::optional<IdentityParts> parseIdentityFraming(std::string_view identity) {
  constexpr std::string_view kPrefix = "pj.source.v1|";
  if (identity.substr(0, kPrefix.size()) != kPrefix) {
    return std::nullopt;
  }
  std::string_view rest = identity.substr(kPrefix.size());
  const size_t length_end = rest.find('|');
  if (length_end == std::string_view::npos) {
    return std::nullopt;
  }
  size_t provider_length = 0;
  const auto parsed = std::from_chars(rest.data(), rest.data() + length_end, provider_length);
  if (parsed.ec != std::errc{} || parsed.ptr != rest.data() + length_end) {
    return std::nullopt;
  }
  rest.remove_prefix(length_end + 1);
  if (rest.size() < provider_length + 1 || rest[provider_length] != '|') {
    return std::nullopt;
  }
  return IdentityParts{
      std::string(rest.substr(0, provider_length)),
      std::string(rest.substr(provider_length + 1)),
  };
}

/// Facts from the already-read summary.
struct FileFacts {
  uint64_t message_count = 0;
  uint64_t channel_count = 0;
  uint64_t schema_count = 0;
  uint64_t chunk_count = 0;
  std::optional<uint64_t> message_start_time_ns;
  std::optional<uint64_t> message_end_time_ns;
  /// Chunk compression when uniform across the file ("" = uncompressed);
  /// unset when the file mixes compressions or has no chunks.
  std::optional<std::string> compression;
};

inline FileFacts gatherFileFacts(mcap::McapReader& reader) {
  FileFacts facts;
  if (const auto& stats = reader.statistics()) {
    facts.message_count = stats->messageCount;
    facts.channel_count = stats->channelCount;
    facts.schema_count = stats->schemaCount;
    if (stats->messageCount > 0) {
      facts.message_start_time_ns = stats->messageStartTime;
      facts.message_end_time_ns = stats->messageEndTime;
    }
  } else {
    facts.channel_count = reader.channels().size();
    facts.schema_count = reader.schemas().size();
  }
  const auto& chunks = reader.chunkIndexes();
  facts.chunk_count = chunks.size();
  if (!chunks.empty()) {
    const std::string& first_compression = chunks.front().compression;
    const bool uniform = std::all_of(chunks.begin(), chunks.end(), [&](const mcap::ChunkIndex& chunk) {
      return chunk.compression == first_compression;
    });
    if (uniform) {
      facts.compression = first_compression;
    }
  }
  return facts;
}

/// The Metadata record `index` points at, or nullopt when it cannot be fetched or parsed.
inline std::optional<mcap::Metadata> readMetadataRecord(
    mcap::IReadable& data_source, const mcap::MetadataIndex& index) {
  mcap::RecordReader record_reader(data_source, index.offset, index.offset + index.length);
  const std::optional<mcap::Record> record = record_reader.next();
  mcap::Metadata metadata;
  if (!record || !record_reader.status().ok() || !mcap::McapReader::ParseMetadata(*record, &metadata).ok()) {
    return std::nullopt;
  }
  return metadata;
}

/// The last readable Metadata record named `name` in file order (a writer that records at open and again at close
/// leaves its final word last), or nullopt. Same index caveat as collectMetadataRecords().
inline std::optional<mcap::Metadata> lastMetadataRecord(mcap::McapReader& reader, std::string_view name) {
  mcap::IReadable* data_source = reader.dataSource();
  if (data_source == nullptr) {
    return std::nullopt;
  }
  std::vector<const mcap::MetadataIndex*> candidates;
  const auto [first, last] = reader.metadataIndexes().equal_range(std::string(name));
  for (auto it = first; it != last; ++it) {
    candidates.push_back(&it->second);
  }
  std::sort(candidates.begin(), candidates.end(), [](const mcap::MetadataIndex* lhs, const mcap::MetadataIndex* rhs) {
    return lhs->offset > rhs->offset;
  });
  for (const mcap::MetadataIndex* index : candidates) {
    if (auto metadata = readMetadataRecord(*data_source, *index)) {
      return metadata;
    }
  }
  return std::nullopt;
}

/// Every indexed file-level Metadata record body, in file order; records the reader cannot fetch or parse are skipped
/// with a diagnostic. A valid summary that OMITS metadata indexes is indistinguishable from one with no metadata (the
/// fallback scan only reconstructs indexes when there is no summary at all): both yield an empty list.
inline std::vector<mcap::Metadata> collectMetadataRecords(
    mcap::McapReader& reader, std::vector<std::string>* diagnostics) {
  std::vector<mcap::Metadata> records;
  mcap::IReadable* data_source = reader.dataSource();
  if (data_source == nullptr) {
    return records;
  }

  // The index multimap is keyed by name: re-sort by file offset so repeated names (pj.recording open/close) keep their
  // write order.
  std::vector<const mcap::MetadataIndex*> ordered;
  for (const auto& [name, index] : reader.metadataIndexes()) {
    ordered.push_back(&index);
  }
  std::sort(ordered.begin(), ordered.end(), [](const mcap::MetadataIndex* lhs, const mcap::MetadataIndex* rhs) {
    return lhs->offset < rhs->offset;
  });

  for (const mcap::MetadataIndex* index : ordered) {
    std::optional<mcap::Metadata> metadata = readMetadataRecord(*data_source, *index);
    if (!metadata) {
      if (diagnostics != nullptr) {
        diagnostics->push_back("MCAP metadata record '" + index->name + "' could not be read; skipped");
      }
      continue;
    }
    records.push_back(std::move(*metadata));
  }
  return records;
}

namespace detail {

inline size_t entriesByteSize(const mcap::KeyValueMap& entries) {
  size_t total = 0;
  for (const auto& [key, value] : entries) {
    total += key.size() + value.size();
  }
  return total;
}

}  // namespace detail

/// Pure (no file I/O), so unit-testable.
inline nlohmann::json buildDocument(
    const FileFacts& facts, const std::vector<mcap::Metadata>& records, std::vector<std::string>* diagnostics) {
  nlohmann::json document = nlohmann::json::object();

  nlohmann::json& file = document["file"];
  file["message_count"] = facts.message_count;
  file["channel_count"] = facts.channel_count;
  file["schema_count"] = facts.schema_count;
  file["chunk_count"] = facts.chunk_count;
  if (facts.message_start_time_ns.has_value()) {
    file["message_start_time_ns"] = *facts.message_start_time_ns;
    file["message_end_time_ns"] = *facts.message_end_time_ns;
  }
  if (facts.compression.has_value()) {
    file["compression"] = facts.compression->empty() ? "none" : *facts.compression;
  }

  const mcap::Metadata* last_recording = nullptr;
  const mcap::Metadata* first_capture = nullptr;

  if (!records.empty()) {
    nlohmann::json& raw_records = document["mcap_metadata"];
    raw_records = nlohmann::json::array();
    for (const mcap::Metadata& record : records) {
      nlohmann::json entry = nlohmann::json::object();
      entry["name"] = record.name;
      if (detail::entriesByteSize(record.metadata) > kMaxRecordBytes) {
        entry["skipped"] = "record exceeds " + std::to_string(kMaxRecordBytes) + " bytes";
        if (diagnostics != nullptr) {
          diagnostics->push_back("MCAP metadata record '" + record.name + "' exceeds the size ceiling; elided");
        }
        raw_records.push_back(std::move(entry));
        continue;
      }
      nlohmann::json& entries = entry["entries"];
      entries = nlohmann::json::object();
      for (const auto& [key, value] : record.metadata) {
        entries[key] = value;
      }
      raw_records.push_back(std::move(entry));

      if (record.name == "pj.recording") {
        last_recording = &record;  // file order: the closing record wins
      } else if (record.name == "pj.capture" && first_capture == nullptr) {
        first_capture = &record;
      }
    }
  }

  if (last_recording != nullptr) {
    nlohmann::json& recording = document["recording"];
    recording = nlohmann::json::object();
    for (const auto& [key, value] : last_recording->metadata) {
      recording[key] = value;
    }
  }

  if (first_capture != nullptr) {
    const auto json_entry = first_capture->metadata.find("json");
    if (json_entry == first_capture->metadata.end()) {
      if (diagnostics != nullptr) {
        diagnostics->push_back("pj.capture record carries no 'json' entry; capture manifest unavailable");
      }
    } else {
      nlohmann::json manifest = nlohmann::json::parse(json_entry->second, nullptr, false);
      if (!manifest.is_object()) {
        if (diagnostics != nullptr) {
          diagnostics->push_back("pj.capture manifest is not valid JSON; capture details unavailable");
        }
      } else {
        // Recover the verbatim descriptor from the identity framing; the raw identity stays in place in case the
        // framing is unknown.
        if (const auto identity = manifest.find("identity"); identity != manifest.end() && identity->is_string()) {
          if (const auto parts = parseIdentityFraming(identity->get<std::string>())) {
            nlohmann::json descriptor = nlohmann::json::parse(parts->descriptor_json, nullptr, false);
            manifest["descriptor"] =
                descriptor.is_discarded() ? nlohmann::json(parts->descriptor_json) : std::move(descriptor);
          }
        }
        document["capture"] = std::move(manifest);
      }
    }
  }

  return document;
}

/// Facts of a recording split across files (a rosbag2 bag): counts add up and the time envelope widens, except channels
/// and schemas, which every split repeats and so take the largest file's count.
inline FileFacts mergeFileFacts(const FileFacts& a, const FileFacts& b) {
  FileFacts merged;
  merged.message_count = a.message_count + b.message_count;
  merged.channel_count = std::max(a.channel_count, b.channel_count);
  merged.schema_count = std::max(a.schema_count, b.schema_count);
  merged.chunk_count = a.chunk_count + b.chunk_count;
  merged.message_start_time_ns = a.message_start_time_ns;
  merged.message_end_time_ns = a.message_end_time_ns;
  if (b.message_start_time_ns.has_value()) {
    merged.message_start_time_ns =
        std::min(a.message_start_time_ns.value_or(*b.message_start_time_ns), *b.message_start_time_ns);
    merged.message_end_time_ns =
        std::max(a.message_end_time_ns.value_or(*b.message_end_time_ns), *b.message_end_time_ns);
  }
  merged.compression = a.compression == b.compression ? a.compression : std::nullopt;
  return merged;
}

/// `readers`: a single MCAP, or every split of a rosbag2 bag in recording order (non-empty). Facts are merged and
/// metadata records concatenated.
inline nlohmann::json extractDatasetMetadata(
    const std::vector<mcap::McapReader*>& readers, std::vector<std::string>* diagnostics) {
  FileFacts facts = gatherFileFacts(*readers.front());
  std::vector<mcap::Metadata> records = collectMetadataRecords(*readers.front(), diagnostics);
  for (size_t i = 1; i < readers.size(); ++i) {
    facts = mergeFileFacts(facts, gatherFileFacts(*readers[i]));
    auto more = collectMetadataRecords(*readers[i], diagnostics);
    records.insert(records.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
  }
  return buildDocument(facts, records, diagnostics);
}

/// Delivery to the host (set_dataset_metadata, SDK 0.32). A host without the slot returns an error, deliberately
/// ignored: the load is unaffected and that host shows no metadata.
template <typename RuntimeHostView>
inline void publishDatasetMetadata(const RuntimeHostView& host, const nlohmann::json& document) {
  (void)host.setDatasetMetadata(document.dump());
}

}  // namespace PJ::McapMetadata
