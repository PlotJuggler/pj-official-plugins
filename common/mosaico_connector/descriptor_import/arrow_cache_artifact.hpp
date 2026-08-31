// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The cache artifact format: an MCAP container storing EXACTLY what the fetch
// ingested — never a re-encode that could drift. One channel per topic, two
// channel kinds:
//   - SCALAR topics ("arrow_ipc" encoding): the post-normalization /
//     post-timestamp-synthesis Arrow record batches serialized VERBATIM as
//     encapsulated IPC messages; the schema record carries the serialized
//     Arrow schema.
//   - OBJECT topics ("pj_canonical" encoding, no schema record): one message
//     per row holding the CONVERTED canonical object blob exactly as it was
//     pushed to the host's ObjectStore, with the exact ingest timestamp as
//     logTime — replay is byte-identical by construction, and needs no
//     ontology converter at all.
// Channel metadata stores the ingest ROUTING DECISION (ontology tag,
// canonical object metadata, timestamp column) so replay never re-derives
// it; a Metadata record carries the canonical source descriptor (provenance,
// re-hashed by the validator). Artifacts carry the .pjmosaico extension (the
// cache loader's declared extension; it must never be ".mcap" or ordinary
// MCAP drag-drops would see two candidate loaders), but the container IS MCAP
// — the mcap CLI inspects it regardless of extension; payloads are not
// ROS/protobuf messages.
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <pj_base/sdk/descriptor_import/request_cache.hpp>
#include <string>
#include <vector>

namespace arrow {
class RecordBatch;
class Schema;
}  // namespace arrow

namespace mosaico {

/// The companion loader's manifest id (data_load_mosaico_cache/manifest.json)
/// — the producer half of the artifact contract names its consumer here, so
/// the binding lives next to the format instead of as a stray literal.
inline constexpr const char* kMosaicoCacheLoaderPluginId = "mosaico-cache-loader";

/// Metadata record names inside an artifact.
inline constexpr const char* kProvenanceMetadataName = "mosaico/source_descriptor";

/// Per-topic replay information, stored in the channel metadata.
struct ArtifactTopic {
  std::string name;                // bare topic name (dataset carries the sequence)
  std::string ontology_tag;        // resolved routing decision ("" = scalar route)
  std::string canonical_metadata;  // registerObjectTopic metadata JSON ("" = scalar)
  std::string timestamp_column;    // the column used as the time base ("" = none)
};

/// Streaming artifact writer. Lifecycle: open -> addTopic* -> writeBatch* ->
/// close. Not thread-safe; one writer per partial file. The descriptor is
/// embedded at open() (before any message, so it never splits a chunk).
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

  /// Register a SCALAR topic; returns the channel id to pass to writeBatch.
  [[nodiscard]] std::optional<std::uint16_t> addTopic(
      const ArtifactTopic& topic, const arrow::Schema& schema, std::string* error);

  /// Append one record batch (serialized as one MCAP message). `log_time_ns`
  /// is the batch's representative timestamp (first row's, by convention).
  [[nodiscard]] bool writeBatch(
      std::uint16_t channel_id, const arrow::RecordBatch& batch, std::int64_t log_time_ns, std::string* error);

  /// Register an OBJECT topic (canonical-blob channel, no Arrow schema);
  /// returns the channel id to pass to writeObjectSample.
  [[nodiscard]] std::optional<std::uint16_t> addObjectTopic(const ArtifactTopic& topic, std::string* error);

  /// Append one canonical object blob (one MCAP message = one row), with the
  /// EXACT timestamp the row was pushed to the host with.
  [[nodiscard]] bool writeObjectSample(
      std::uint16_t channel_id, std::int64_t log_time_ns, const std::uint8_t* data, std::size_t size,
      std::string* error);

  /// Close the container, writing its footer and summary section.
  [[nodiscard]] bool close(std::string* error);

  /// Abort: close the underlying file WITHOUT a valid footer. The caller
  /// deletes the partial (RequestArtifactCache::commit would reject it anyway).
  void abort();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// The RequestArtifactCache validator for this artifact format. Bounded I/O
/// (raw footer preflight + summary section only — a forged file can never
/// make the parser allocate past the fixed budgets): footer + summary
/// readable, Statistics present, and embedded provenance present with its
/// bytes RE-HASHING to `hex`. Fetch-ledger completeness is enforced before
/// this artifact is finalized and published.
[[nodiscard]] bool validateArtifact(const std::filesystem::path& file, const std::string& hex, std::string* error);

/// The env-resolved standard cache root: MOSAICO_CACHE_DIR ||
/// $XDG_CACHE_HOME/mosaico/sessions || ~/.cache/mosaico/sessions. Empty (with
/// `error` set) when unresolvable. The dialog shows this as the cache-directory
/// placeholder hint while an empty configured root keeps this resolution live.
[[nodiscard]] std::filesystem::path standardCacheRoot(std::string* error);

/// The one "configured directory else standard root" rule, paired with the
/// Mosaico artifact suffix, identity scheme, and provenance-rehashing
/// validator. Every capture/query/import consumer uses this factory so an
/// identity always names the same validated cache entry.
[[nodiscard]] PJ::sdk::descriptor_import::RequestArtifactCache makeArtifactCache(
    const std::filesystem::path& configured_root = {}, std::string* error = nullptr);

/// Cache budget when `mosaico/cache_max_gb` is unset: 20 GiB.
inline constexpr double kDefaultCacheMaxGb = 20.0;

/// The SDK cleanup policy for a `max_gb` budget: non-positive or non-finite
/// means unlimited; otherwise max_total_bytes = max_gb GiB (saturating).
/// Orphaned partials keep the SDK's default age.
[[nodiscard]] PJ::sdk::descriptor_import::CleanupPolicy cacheCleanupPolicy(double max_gb);

/// Best-effort maintenance at the moments the cache may have grown: the
/// SDK's LRU cleanup under `policy`. Never throws. Leased and in-flight
/// artifacts are never evicted (the SDK's rule), so running it while holding
/// a just-published artifact's lease is the intended way to protect it.
[[nodiscard]] PJ::sdk::descriptor_import::CleanupResult maintainCache(
    PJ::sdk::descriptor_import::RequestArtifactCache& cache,
    const PJ::sdk::descriptor_import::CleanupPolicy& policy) noexcept;

/// A path as the ABI's UTF-8 string — never through path::string(), which
/// narrows via the execution code page on Windows.
[[nodiscard]] std::string utf8Path(const std::filesystem::path& path);

/// Quarantine an artifact whose BODY failed replay after the bounded
/// validation passed: rename it to "<artifact>.corrupt" (best-effort) so the
/// next layout open classifies the identity as a miss and re-downloads
/// instead of repeating the failure forever. False = rename failed, file
/// left in place.
bool quarantineArtifact(const std::filesystem::path& artifact, std::string* error);

/// One canonical object blob, with the exact timestamp it was ingested at.
struct ArtifactObjectSample {
  std::int64_t log_time_ns = 0;
  std::vector<std::uint8_t> payload;
};

/// One topic's replayable content, as read back from an artifact. Exactly one
/// of the two representations is populated, per `is_object`.
struct ArtifactTopicData {
  ArtifactTopic info;
  bool is_object = false;
  // Scalar channels:
  std::shared_ptr<arrow::Schema> schema;
  std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
  std::vector<std::int64_t> batch_log_times_ns;  // parallel to `batches`
  // Object channels:
  std::vector<ArtifactObjectSample> object_samples;
};

/// Summary-only description of one artifact channel. `message_count` comes
/// from the MCAP Statistics record and is used to report replay progress
/// before any message payload is read.
struct ArtifactTopicSummary {
  ArtifactTopic info;
  bool is_object = false;
  std::shared_ptr<arrow::Schema> schema;  // scalar channels only
  std::uint64_t message_count = 0;
};

/// Allocation-safe raw framing gate for the replay reader. Call this before
/// any other MCAP consumer in a cache-loader path: it bounds the Header and
/// summary section and rejects Chunk records embedded in the summary.
[[nodiscard]] bool preflightArtifactForReplay(const std::filesystem::path& file, std::string* error);

/// Incremental artifact reader used by cache replay. `open()` reads only the
/// bounded summary and channel schemas. Each `startTopic()` visits, in file
/// order, only summary-indexed chunks containing that channel. One chunk and
/// the current decoded batch/blob are retained at a time. A topic must be
/// consumed synchronously before starting another one.
class ArtifactStreamReader {
 public:
  ArtifactStreamReader();
  ~ArtifactStreamReader();
  ArtifactStreamReader(const ArtifactStreamReader&) = delete;
  ArtifactStreamReader& operator=(const ArtifactStreamReader&) = delete;

  [[nodiscard]] bool open(
      const std::filesystem::path& file, std::string* out_canonical_descriptor_json, std::string* error);
  void close();

  [[nodiscard]] const std::vector<ArtifactTopicSummary>& topics() const;
  [[nodiscard]] std::uint64_t messageCount() const;

  [[nodiscard]] bool startTopic(std::size_t topic_index, std::string* error);

  /// `out_batch == nullptr` means end of the selected scalar topic.
  [[nodiscard]] bool readNextScalar(
      std::shared_ptr<arrow::RecordBatch>* out_batch, std::int64_t* out_log_time_ns, std::string* error);

  /// `out_has_sample == false` means end of the selected object topic.
  [[nodiscard]] bool readNextObject(ArtifactObjectSample* out_sample, bool* out_has_sample, std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Read a whole artifact back for replay. Returns the topics in channel-id
/// order and the embedded canonical descriptor JSON. Fails (never partially
/// succeeds) on any structural problem; run validateArtifact first when the
/// file's provenance matters.
[[nodiscard]] bool readArtifact(
    const std::filesystem::path& file, std::vector<ArtifactTopicData>* out_topics,
    std::string* out_canonical_descriptor_json, std::string* error);

}  // namespace mosaico
