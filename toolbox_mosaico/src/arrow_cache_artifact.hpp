// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The cache artifact format: an MCAP container whose message payloads are the
// fetch's Arrow record batches serialized VERBATIM as encapsulated IPC
// messages — no per-row re-encode, ever. One channel per topic; the channel
// metadata stores the ingest ROUTING DECISION (ontology tag, canonical object
// metadata, timestamp column) so replay never re-derives it; the schema
// record carries the serialized Arrow schema; two Metadata records carry the
// canonical source descriptor (provenance, re-hashed by the validator) and
// the content summary (the finalize completeness pin). The .mcap extension is
// honest — standard MCAP tooling can inspect a cache artifact — but payloads
// are Arrow IPC ("arrow_ipc" message encoding), not ROS/protobuf messages.
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "session_file_cache.hpp"

namespace arrow {
class RecordBatch;
class Schema;
}  // namespace arrow

namespace mosaico {

/// Metadata record names inside an artifact.
inline constexpr const char* kProvenanceMetadataName = "mosaico/source_descriptor";
inline constexpr const char* kContentSummaryMetadataName = "mosaico/content_summary";

/// Per-topic replay information, stored in the channel metadata.
struct ArtifactTopic {
  std::string name;                // bare topic name (dataset carries the sequence)
  std::string ontology_tag;        // resolved routing decision ("" = scalar route)
  std::string canonical_metadata;  // registerObjectTopic metadata JSON ("" = scalar)
  std::string timestamp_column;    // the column used as the time base ("" = none)
};

/// Streaming artifact writer. Lifecycle: open -> addTopic* -> writeBatch* ->
/// close. Not thread-safe; one writer per partial file. The descriptor is
/// embedded at open() (before any message, so it never splits a chunk); the
/// content summary is embedded at close(), which also reports the counts the
/// caller passes to SessionFileCache::finalize as ExpectedContent.
class ArtifactWriter {
 public:
  ArtifactWriter();
  ~ArtifactWriter();
  ArtifactWriter(const ArtifactWriter&) = delete;
  ArtifactWriter& operator=(const ArtifactWriter&) = delete;

  /// Create `path` (truncating) and embed `canonical_descriptor_json` as the
  /// provenance record. The canonical bytes are written VERBATIM — they are
  /// the identity's digest input and must never be re-serialized.
  [[nodiscard]] bool open(
      const std::filesystem::path& path, const std::string& canonical_descriptor_json, std::string* error);

  /// Register a topic; returns the channel id to pass to writeBatch.
  [[nodiscard]] std::optional<std::uint16_t> addTopic(
      const ArtifactTopic& topic, const arrow::Schema& schema, std::string* error);

  /// Append one record batch (serialized as one MCAP message). `log_time_ns`
  /// is the batch's representative timestamp (first row's, by convention).
  [[nodiscard]] bool writeBatch(
      std::uint16_t channel_id, const arrow::RecordBatch& batch, std::int64_t log_time_ns, std::string* error);

  /// Embed the content summary, close the container (footer + summary
  /// section), and report the producer counts for the finalize pin.
  [[nodiscard]] bool close(SessionFileCache::ExpectedContent* out_expected, std::string* error);

  /// Abort: close the underlying file WITHOUT a valid footer. The caller
  /// deletes the partial (SessionFileCache::finalize would reject it anyway).
  void abort();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// The SessionFileCache validator for this artifact format. Bounded I/O
/// (raw footer preflight + summary section only — a forged file can never
/// make the parser allocate past the fixed budgets): footer + summary
/// readable, Statistics present, embedded provenance present with its bytes
/// RE-HASHING to `hex`, and — when `expected` is provided (the finalize
/// gate) — the embedded content summary matching the producer counts exactly.
[[nodiscard]] bool validateArtifact(
    const std::filesystem::path& file, const std::string& hex,
    const std::optional<SessionFileCache::ExpectedContent>& expected, std::string* error);

/// One topic's replayable content, as read back from an artifact.
struct ArtifactTopicData {
  ArtifactTopic info;
  std::shared_ptr<arrow::Schema> schema;
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
  std::vector<std::int64_t> batch_log_times_ns;  // parallel to `batches`
};

/// Read a whole artifact back for replay. Returns the topics in channel-id
/// order and the embedded canonical descriptor JSON. Fails (never partially
/// succeeds) on any structural problem; run validateArtifact first when the
/// file's provenance matters.
[[nodiscard]] bool readArtifact(
    const std::filesystem::path& file, std::vector<ArtifactTopicData>* out_topics,
    std::string* out_canonical_descriptor_json, std::string* error);

}  // namespace mosaico
