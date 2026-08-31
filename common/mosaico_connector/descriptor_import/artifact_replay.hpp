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
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "descriptor_import/arrow_cache_artifact.hpp"

struct ArrowArrayStream;

namespace mosaico {

/// The write primitives replay needs. Every sink reports failure through its
/// return value + `error`; a failed sink aborts the replay with that error.
struct ReplaySinks {
  /// Optional replay-start notification after the summary is read but before
  /// any payload. Returning false aborts replay with the supplied error.
  std::function<bool(std::size_t topics_total, std::uint64_t messages_total, std::string* error)> start;

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

  /// Optional cancellation poll, checked before each object message or scalar
  /// batch is read. Returning true stops replay promptly.
  std::function<bool()> is_cancelled;

  /// Optional per-message/batch progress tick. Returning false stops replay:
  /// replayArtifact returns false with a "stopped" error.
  std::function<bool(std::uint64_t messages_done, std::uint64_t messages_total)> progress;
};

/// Replay every topic of `topics` (as returned by readArtifact) through the
/// sinks. Fails on the first sink error; never partially skips a topic
/// silently.
[[nodiscard]] bool replayArtifact(
    const std::vector<ArtifactTopicData>& topics, const ReplaySinks& sinks, std::string* error);

/// Stream an artifact directly from MCAP through the sinks. Topics are visited
/// in channel-id order; each channel visits its indexed chunks in file order,
/// retaining one decompressed chunk at a time. Scalar sinks receive one lazy
/// Arrow C stream per topic, preserving the host-facing replay contract.
[[nodiscard]] bool replayArtifact(const std::filesystem::path& file, const ReplaySinks& sinks, std::string* error);

/// Loader-facing classification: an explicit replay stop, or a host stop that
/// raced with another replay error, is reported as user cancellation.
[[nodiscard]] bool isReplayCancellation(std::string_view replay_error, bool stop_requested);

}  // namespace mosaico
