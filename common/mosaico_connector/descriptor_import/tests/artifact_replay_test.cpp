// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Replay determinism against recording sinks: scalar topics come back as the
// exact stored batches (round-tripped through the Arrow C stream the real
// appendArrowStream consumes), object topics as the exact blobs at the exact
// timestamps, plus error propagation and the progress-stop contract.
#include "descriptor_import/artifact_replay.hpp"

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace {

using mosaico::ArtifactObjectSample;
using mosaico::ArtifactTopic;
using mosaico::ArtifactTopicData;
using mosaico::ReplaySinks;

std::shared_ptr<arrow::RecordBatch> scalarBatch(std::int64_t t0, int rows) {
  arrow::Int64Builder t_builder;
  arrow::DoubleBuilder v_builder;
  for (int i = 0; i < rows; ++i) {
    EXPECT_TRUE(t_builder.Append(t0 + i).ok());
    EXPECT_TRUE(v_builder.Append(static_cast<double>(i) * 0.5).ok());
  }
  std::shared_ptr<arrow::Array> t_array;
  std::shared_ptr<arrow::Array> v_array;
  EXPECT_TRUE(t_builder.Finish(&t_array).ok());
  EXPECT_TRUE(v_builder.Finish(&v_array).ok());
  const auto schema =
      arrow::schema({arrow::field("timestamp_ns", arrow::int64()), arrow::field("value", arrow::float64())});
  return arrow::RecordBatch::Make(schema, rows, {t_array, v_array});
}

ArtifactTopicData scalarTopic(const std::string& name) {
  ArtifactTopicData topic;
  topic.info =
      ArtifactTopic{.name = name, .ontology_tag = "", .canonical_metadata = "", .timestamp_column = "timestamp_ns"};
  topic.batches = {scalarBatch(100, 3), scalarBatch(200, 2)};
  topic.batch_log_times_ns = {100, 200};
  topic.schema = topic.batches[0]->schema();
  return topic;
}

ArtifactTopicData objectTopic(const std::string& name) {
  ArtifactTopicData topic;
  topic.info = ArtifactTopic{
      .name = name,
      .ontology_tag = "image",
      .canonical_metadata = R"({"builtin_object_type":"kImage"})",
      .timestamp_column = ""};
  topic.is_object = true;
  topic.object_samples = {{150, std::vector<std::uint8_t>(16, 0xAB)}, {250, std::vector<std::uint8_t>(8, 0xCD)}};
  return topic;
}

// Recording sinks: scalar streams are imported back through the same Arrow C
// bridge the real appendArrowStream uses, so ownership/release is exercised.
struct Recorder {
  struct ScalarCapture {
    std::string timestamp_column;
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
  };
  std::map<std::string, ScalarCapture> scalars;
  std::map<std::uint64_t, std::string> handles;
  std::map<std::string, std::vector<ArtifactObjectSample>> objects;
  std::uint64_t next_handle = 1;

  ReplaySinks sinks() {
    ReplaySinks s;
    s.append_scalar_stream = [this](
                                 const std::string& topic, ArrowArrayStream* stream,
                                 const std::string& timestamp_column, std::string* error) {
      auto reader = arrow::ImportRecordBatchReader(stream);
      if (!reader.ok()) {
        *error = reader.status().ToString();
        return false;
      }
      ScalarCapture capture;
      capture.timestamp_column = timestamp_column;
      while (true) {
        auto batch = (*reader)->Next();
        if (!batch.ok()) {
          *error = batch.status().ToString();
          return false;
        }
        if (*batch == nullptr) {
          break;
        }
        capture.batches.push_back(*batch);
      }
      scalars[topic] = std::move(capture);
      return true;
    };
    s.register_object_topic =
        [this](const std::string& topic, const std::string& metadata, std::string*) -> std::optional<std::uint64_t> {
      EXPECT_FALSE(metadata.empty());
      const std::uint64_t handle = next_handle++;
      handles[handle] = topic;
      return handle;
    };
    s.push_object =
        [this](std::uint64_t handle, std::int64_t ts_ns, const std::uint8_t* data, std::size_t size, std::string*) {
          objects[handles.at(handle)].push_back({ts_ns, std::vector<std::uint8_t>(data, data + size)});
          return true;
        };
    return s;
  }
};

}  // namespace

TEST(ArtifactReplay, ScalarAndObjectTopicsReplayExactly) {
  const std::vector<ArtifactTopicData> topics = {scalarTopic("/imu"), objectTopic("/blob")};
  Recorder recorder;
  std::string error;
  ASSERT_TRUE(mosaico::replayArtifact(topics, recorder.sinks(), &error)) << error;

  ASSERT_EQ(recorder.scalars.size(), 1u);
  const auto& imu = recorder.scalars.at("/imu");
  EXPECT_EQ(imu.timestamp_column, "timestamp_ns");
  ASSERT_EQ(imu.batches.size(), 2u);
  EXPECT_TRUE(imu.batches[0]->Equals(*scalarBatch(100, 3)));
  EXPECT_TRUE(imu.batches[1]->Equals(*scalarBatch(200, 2)));

  ASSERT_EQ(recorder.objects.size(), 1u);
  const auto& blob = recorder.objects.at("/blob");
  ASSERT_EQ(blob.size(), 2u);
  EXPECT_EQ(blob[0].log_time_ns, 150);
  EXPECT_EQ(blob[0].payload, std::vector<std::uint8_t>(16, 0xAB));
  EXPECT_EQ(blob[1].log_time_ns, 250);
  EXPECT_EQ(blob[1].payload, std::vector<std::uint8_t>(8, 0xCD));
}

TEST(ArtifactReplay, SinkFailureAbortsWithTheSinkError) {
  Recorder recorder;
  ReplaySinks sinks = recorder.sinks();
  sinks.push_object = [](std::uint64_t, std::int64_t, const std::uint8_t*, std::size_t, std::string* error) {
    *error = "disk full";
    return false;
  };
  std::string error;
  EXPECT_FALSE(mosaico::replayArtifact({objectTopic("/blob")}, sinks, &error));
  EXPECT_NE(error.find("disk full"), std::string::npos) << error;
}

TEST(ArtifactReplay, ProgressStopAborts) {
  Recorder recorder;
  ReplaySinks sinks = recorder.sinks();
  std::vector<std::pair<std::size_t, std::size_t>> ticks;
  sinks.progress = [&ticks](std::size_t done, std::size_t total) {
    ticks.push_back({done, total});
    return done < 1;  // stop after the first topic
  };
  std::string error;
  EXPECT_FALSE(mosaico::replayArtifact({scalarTopic("/a"), scalarTopic("/b")}, sinks, &error));
  EXPECT_NE(error.find("stopped"), std::string::npos);
  ASSERT_EQ(ticks.size(), 1u);
  EXPECT_EQ(recorder.scalars.size(), 1u);  // the second topic never replayed
}

TEST(ArtifactReplay, MissingSinksFailClosed) {
  std::string error;
  EXPECT_FALSE(mosaico::replayArtifact({scalarTopic("/a")}, ReplaySinks{}, &error));
  EXPECT_NE(error.find("not fully configured"), std::string::npos);
}
