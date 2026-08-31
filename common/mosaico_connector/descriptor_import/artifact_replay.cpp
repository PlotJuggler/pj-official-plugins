// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "descriptor_import/artifact_replay.hpp"

#include <arrow/c/bridge.h>
#include <arrow/record_batch.h>
#include <arrow/status.h>

#include <limits>
#include <memory>
#include <utility>

namespace mosaico {

namespace {

bool fail(std::string* error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  return false;
}

class ReplayProgress {
 public:
  ReplayProgress(const ReplaySinks& sinks, std::uint64_t total) : sinks_(sinks), total_(total) {}

  bool beforeNext() {
    if (pending_delivery_) {
      pending_delivery_ = false;
      ++done_;
      if (sinks_.progress && !sinks_.progress(done_, total_)) {
        stopped_ = true;
        return false;
      }
    }
    if (sinks_.is_cancelled && sinks_.is_cancelled()) {
      stopped_ = true;
      return false;
    }
    return true;
  }

  void delivered() {
    pending_delivery_ = true;
  }

  void readFailed(std::string error) {
    read_error_ = std::move(error);
  }

  [[nodiscard]] bool stopped() const {
    return stopped_;
  }

  [[nodiscard]] const std::string& readError() const {
    return read_error_;
  }

 private:
  const ReplaySinks& sinks_;
  std::uint64_t total_ = 0;
  std::uint64_t done_ = 0;
  bool pending_delivery_ = false;
  bool stopped_ = false;
  std::string read_error_;
};

class LazyReplayBatchReader : public arrow::RecordBatchReader {
 public:
  using Next = std::function<bool(std::shared_ptr<arrow::RecordBatch>*, std::string*)>;

  LazyReplayBatchReader(std::shared_ptr<arrow::Schema> schema, Next next, ReplayProgress* progress)
      : schema_(std::move(schema)), next_(std::move(next)), progress_(progress) {}

  std::shared_ptr<arrow::Schema> schema() const override {
    return schema_;
  }

  arrow::Status ReadNext(std::shared_ptr<arrow::RecordBatch>* batch) override {
    *batch = nullptr;
    // A batch returned by the previous call has now reached the sink. Report
    // it, then poll cancellation before reading/decompressing the next one.
    if (!progress_->beforeNext()) {
      return arrow::Status::Cancelled("replay stopped");
    }
    std::string read_error;
    if (!next_(batch, &read_error)) {
      progress_->readFailed(read_error);
      return arrow::Status::IOError(read_error);
    }
    if (*batch) {
      progress_->delivered();
    }
    return arrow::Status::OK();
  }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  Next next_;
  ReplayProgress* progress_;
};

bool sinksAreConfigured(const ReplaySinks& sinks, std::string* error) {
  if (sinks.append_scalar_stream && sinks.register_object_topic && sinks.push_object) {
    return true;
  }
  return fail(error, "replay sinks are not fully configured");
}

bool addCount(std::uint64_t value, std::uint64_t* total, std::string* error) {
  if (std::numeric_limits<std::uint64_t>::max() - *total < value) {
    return fail(error, "replay message count overflow");
  }
  *total += value;
  return true;
}

bool beginReplay(
    const ReplaySinks& sinks, std::size_t topic_count, std::uint64_t message_count, ReplayProgress* progress,
    std::string* error) {
  std::string start_error;
  if (sinks.start && !sinks.start(topic_count, message_count, &start_error)) {
    return fail(error, start_error.empty() ? "replay start failed" : "replay start failed: " + start_error);
  }
  if (!progress->beforeNext()) {
    return fail(error, "replay stopped");
  }
  return true;
}

bool appendScalar(
    const ArtifactTopicSummary& topic, const ReplaySinks& sinks, ReplayProgress* progress,
    LazyReplayBatchReader::Next next, std::string* error) {
  if (!topic.schema) {
    return fail(error, "scalar topic \"" + topic.info.name + "\" has no schema");
  }
  auto reader = std::make_shared<LazyReplayBatchReader>(topic.schema, std::move(next), progress);
  ArrowArrayStream stream;
  const auto exported = arrow::ExportRecordBatchReader(reader, &stream);
  if (!exported.ok()) {
    return fail(error, "scalar topic \"" + topic.info.name + "\" export: " + exported.ToString());
  }
  std::string sink_error;
  const bool appended = sinks.append_scalar_stream(topic.info.name, &stream, topic.info.timestamp_column, &sink_error);
  if (progress->stopped()) {
    return fail(error, "replay stopped");
  }
  if (!progress->readError().empty()) {
    return fail(error, "scalar read failed on \"" + topic.info.name + "\": " + progress->readError());
  }
  if (!appended) {
    return fail(error, "scalar append failed on \"" + topic.info.name + "\": " + sink_error);
  }
  return true;
}

template <typename NextObject>
bool appendObjects(
    const ArtifactTopic& topic, const ReplaySinks& sinks, ReplayProgress* progress, NextObject&& next,
    std::string* error) {
  std::string sink_error;
  const auto handle = sinks.register_object_topic(topic.name, topic.canonical_metadata, &sink_error);
  if (!handle.has_value()) {
    return fail(error, "object topic \"" + topic.name + "\" registration failed: " + sink_error);
  }
  while (true) {
    if (!progress->beforeNext()) {
      return fail(error, "replay stopped");
    }
    const ArtifactObjectSample* sample = nullptr;
    if (!next(&sample, error)) {
      return false;
    }
    if (!sample) {
      return true;
    }
    if (!sinks.push_object(*handle, sample->log_time_ns, sample->payload.data(), sample->payload.size(), &sink_error)) {
      return fail(error, "object push failed on \"" + topic.name + "\": " + sink_error);
    }
    progress->delivered();
  }
}

}  // namespace

bool isReplayCancellation(std::string_view replay_error, bool stop_requested) {
  return stop_requested || replay_error == "replay stopped";
}

bool replayArtifact(const std::vector<ArtifactTopicData>& topics, const ReplaySinks& sinks, std::string* error) {
  if (!sinksAreConfigured(sinks, error)) {
    return false;
  }
  std::uint64_t message_count = 0;
  for (const auto& topic : topics) {
    const auto count = topic.is_object ? topic.object_samples.size() : topic.batches.size();
    if (!addCount(static_cast<std::uint64_t>(count), &message_count, error)) {
      return false;
    }
  }
  ReplayProgress progress(sinks, message_count);
  if (!beginReplay(sinks, topics.size(), message_count, &progress, error)) {
    return false;
  }

  for (const ArtifactTopicData& topic : topics) {
    if (topic.is_object) {
      std::size_t next_sample = 0;
      if (!appendObjects(
              topic.info, sinks, &progress,
              [&topic, &next_sample](const ArtifactObjectSample** sample, std::string*) {
                *sample = next_sample == topic.object_samples.size() ? nullptr : &topic.object_samples[next_sample++];
                return true;
              },
              error)) {
        return false;
      }
    } else {
      std::size_t next_batch = 0;
      ArtifactTopicSummary summary{
          .info = topic.info,
          .is_object = false,
          .schema = topic.schema,
          .message_count = static_cast<std::uint64_t>(topic.batches.size())};
      if (!appendScalar(
              summary, sinks, &progress,
              [&topic, &next_batch](std::shared_ptr<arrow::RecordBatch>* batch, std::string*) {
                if (next_batch != topic.batches.size()) {
                  *batch = topic.batches[next_batch++];
                }
                return true;
              },
              error)) {
        return false;
      }
    }
  }
  return true;
}

bool replayArtifact(const std::filesystem::path& file, const ReplaySinks& sinks, std::string* error) {
  if (!sinksAreConfigured(sinks, error)) {
    return false;
  }
  ArtifactStreamReader artifact;
  if (!artifact.open(file, /*out_canonical_descriptor_json=*/nullptr, error)) {
    return false;
  }
  ReplayProgress progress(sinks, artifact.messageCount());
  if (!beginReplay(sinks, artifact.topics().size(), artifact.messageCount(), &progress, error)) {
    return false;
  }

  for (std::size_t topic_index = 0; topic_index < artifact.topics().size(); ++topic_index) {
    const ArtifactTopicSummary& topic = artifact.topics()[topic_index];
    // This poll occurs before startTopic, so cancellation never needlessly
    // decompresses the selected topic's first chunk.
    if (!progress.beforeNext()) {
      return fail(error, "replay stopped");
    }
    if (!artifact.startTopic(topic_index, error)) {
      return false;
    }
    if (topic.is_object) {
      ArtifactObjectSample sample;
      if (!appendObjects(
              topic.info, sinks, &progress,
              [&artifact, &sample](const ArtifactObjectSample** next_sample, std::string* read_error) {
                bool has_sample = false;
                if (!artifact.readNextObject(&sample, &has_sample, read_error)) {
                  return false;
                }
                *next_sample = has_sample ? &sample : nullptr;
                return true;
              },
              error)) {
        return false;
      }
    } else {
      if (!appendScalar(
              topic, sinks, &progress,
              [&artifact](std::shared_ptr<arrow::RecordBatch>* batch, std::string* read_error) {
                return artifact.readNextScalar(batch, /*out_log_time_ns=*/nullptr, read_error);
              },
              error)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace mosaico
