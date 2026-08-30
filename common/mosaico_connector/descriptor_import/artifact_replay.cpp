// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "descriptor_import/artifact_replay.hpp"

#include <arrow/c/bridge.h>
#include <arrow/record_batch.h>

namespace mosaico {

bool replayArtifact(const std::vector<ArtifactTopicData>& topics, const ReplaySinks& sinks, std::string* error) {
  const auto fail = [&](std::string message) {
    if (error) {
      *error = std::move(message);
    }
    return false;
  };
  if (!sinks.append_scalar_stream || !sinks.register_object_topic || !sinks.push_object) {
    return fail("replay sinks are not fully configured");
  }
  std::size_t done = 0;
  for (const ArtifactTopicData& topic : topics) {
    if (topic.is_object) {
      std::string sink_error;
      const auto handle = sinks.register_object_topic(topic.info.name, topic.info.canonical_metadata, &sink_error);
      if (!handle.has_value()) {
        return fail("object topic \"" + topic.info.name + "\" registration failed: " + sink_error);
      }
      for (const ArtifactObjectSample& sample : topic.object_samples) {
        if (!sinks.push_object(
                *handle, sample.log_time_ns, sample.payload.data(), sample.payload.size(), &sink_error)) {
          return fail("object push failed on \"" + topic.info.name + "\": " + sink_error);
        }
      }
    } else {
      if (!topic.schema) {
        return fail("scalar topic \"" + topic.info.name + "\" has no schema");
      }
      auto reader = arrow::RecordBatchReader::Make(topic.batches, topic.schema);
      if (!reader.ok()) {
        return fail("scalar topic \"" + topic.info.name + "\" reader: " + reader.status().ToString());
      }
      ArrowArrayStream stream;
      const auto exported = arrow::ExportRecordBatchReader(*reader, &stream);
      if (!exported.ok()) {
        return fail("scalar topic \"" + topic.info.name + "\" export: " + exported.ToString());
      }
      // The sink OWNS the stream from here (release on every path).
      std::string sink_error;
      if (!sinks.append_scalar_stream(topic.info.name, &stream, topic.info.timestamp_column, &sink_error)) {
        return fail("scalar append failed on \"" + topic.info.name + "\": " + sink_error);
      }
    }
    ++done;
    if (sinks.progress && !sinks.progress(done, topics.size())) {
      return fail("replay stopped");
    }
  }
  return true;
}

}  // namespace mosaico
