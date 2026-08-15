// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Per-Download cache tee: records exactly what the fetch ingests into a
// session cache artifact (scalar topics as the normalized Arrow batches,
// object topics as the canonical blobs), then finalizes it for promotion.
// The tee is best-effort BY CONTRACT: any failure — lock contention, disk,
// writer error — makes the session inert and the fetch continues eager-only;
// a tee problem never fails an ingest. All write methods must be called
// under the worker's host-write mutex (the same serialization the datastore
// writes already use); finish() runs after every per-topic callback settled.
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "arrow_cache_artifact.hpp"
#include "core/file_lock.h"
#include "session_file_cache.hpp"
#include "source_descriptor.hpp"

namespace arrow {
class RecordBatch;
class Schema;
}  // namespace arrow

namespace mosaico {

class CacheTeeSession {
 public:
  /// Arms the tee for `descriptor`: cache root (the user-configured
  /// `cache_root_override` when non-empty, else standardCacheRoot),
  /// materialize lock, writer over the partial with the canonical descriptor
  /// embedded. On any failure the session constructs INERT (armed() == false)
  /// with the reason in disarmReason().
  explicit CacheTeeSession(const SourceDescriptor& descriptor, const std::filesystem::path& cache_root_override = {});
  ~CacheTeeSession();
  CacheTeeSession(const CacheTeeSession&) = delete;
  CacheTeeSession& operator=(const CacheTeeSession&) = delete;

  [[nodiscard]] bool armed() const;
  [[nodiscard]] const std::string& disarmReason() const;
  [[nodiscard]] const std::string& identity() const;
  [[nodiscard]] const std::string& descriptorJson() const;  ///< with display_name

  /// Record one ingested scalar topic: its normalized schema, timestamp
  /// column, and the exact batches pumped to the host. No-op when inert;
  /// any write failure disarms the session.
  void teeScalarTopic(
      const std::string& topic, const arrow::Schema& schema, const std::string& ts_field,
      const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches);

  /// A per-blob tap for ObjectIngestContext::tee: lazily registers the object
  /// channel on first blob, then records every (ts, blob). The returned
  /// closure is safe to install even when the session is inert (it no-ops)
  /// and never throws.
  [[nodiscard]] std::function<void(std::int64_t, const std::uint8_t*, std::size_t)> objectTee(
      const std::string& topic, const std::string& ontology_tag, const std::string& canonical_metadata);

  /// Terminal. On `complete` (every topic succeeded and the session is still
  /// armed): close the writer, finalize into the cache, and hand back the
  /// artifact path + a dataset-lifetime read lease + the descriptor json for
  /// the promotion request. Anything else aborts and deletes the partial.
  struct Finalized {
    std::filesystem::path path;
    std::optional<FileLock> lease;  ///< nullopt if the shared-lease downgrade failed
  };
  [[nodiscard]] std::optional<Finalized> finish(bool complete);

 private:
  void disarm(std::string reason);

  SessionFileCache cache_;
  std::optional<SessionFileCache::MaterializeLock> lock_;
  ArtifactWriter writer_;
  std::unordered_map<std::string, std::uint16_t> object_channels_;
  std::string identity_;
  std::string descriptor_json_;
  std::string disarm_reason_;
  bool armed_ = false;
  bool finished_ = false;
};

}  // namespace mosaico
