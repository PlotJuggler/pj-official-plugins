// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Replays a read-back cache artifact into a datastore through injected sinks.
// The sinks are plain std::functions rather than a concrete host view because
// the two consumers hold DIFFERENT ABI views over the same primitives: the
// cache loader plugin writes through SourceWriteHostView /
// SourceObjectWriteHostView, while tests record hermetically. Replay is
// deterministic by construction — scalar topics re-append the exact stored
// batches, object topics re-push the exact stored blobs at their exact
// timestamps.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "descriptor_import/arrow_cache_artifact.hpp"

struct ArrowArrayStream;

namespace mosaico {

/// The write primitives replay needs. Every sink reports failure through its
/// return value + `error`; a failed sink aborts the replay with that error.
struct ReplaySinks {
  /// Scalar topic: bare topic name, an OWNING Arrow C stream over the topic's
  /// batches, and the timestamp column ("" = none). The sink takes ownership
  /// of the stream and must release it on every path (appendArrowStream's
  /// contract does exactly that).
  std::function<bool(
      const std::string& topic, ArrowArrayStream* stream, const std::string& timestamp_column, std::string* error)>
      append_scalar_stream;

  /// Object topic registration; the returned opaque handle feeds push_object.
  std::function<std::optional<std::uint64_t>(
      const std::string& topic, const std::string& metadata_json, std::string* error)>
      register_object_topic;

  /// One canonical blob at its exact ingest timestamp.
  std::function<bool(
      std::uint64_t handle, std::int64_t ts_ns, const std::uint8_t* data, std::size_t size, std::string* error)>
      push_object;

  /// Optional per-topic progress tick (after each topic completes). Returning
  /// false stops the replay: replayArtifact returns false with a
  /// "stopped" error, and the caller decides keep-vs-discard.
  std::function<bool(std::size_t topics_done, std::size_t topics_total)> progress;
};

/// Replay every topic of `topics` (as returned by readArtifact) through the
/// sinks. Fails on the first sink error; never partially skips a topic
/// silently.
[[nodiscard]] bool replayArtifact(
    const std::vector<ArtifactTopicData>& topics, const ReplaySinks& sinks, std::string* error);

}  // namespace mosaico
