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

#include <filesystem>
#include <fstream>
#include <map>
#include <mcap/reader.hpp>
#include <string>
#include <vector>

#include "descriptor_import/tests/test_support_fs.hpp"

namespace {

using mosaico::ArtifactObjectSample;
using mosaico::ArtifactTopic;
using mosaico::ArtifactTopicData;
using mosaico::ReplaySinks;

namespace fs = std::filesystem;

struct TempRoot : mosaico_test::ScopedTempDir {
  explicit TempRoot(const std::string& name) : ScopedTempDir("mosaico-replay-test-" + name) {}
};

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

void writeStreamingArtifact(const fs::path& path, int imu_batches = 3, int object_samples = 3) {
  mosaico::ArtifactWriter writer;
  std::string error;
  ASSERT_TRUE(writer.open(path, R"({"kind":"streaming-replay-test"})", &error)) << error;
  const auto imu = writer.addTopic(
      ArtifactTopic{.name = "/imu", .ontology_tag = "", .canonical_metadata = "", .timestamp_column = "timestamp_ns"},
      *scalarBatch(0, 1)->schema(), &error);
  ASSERT_TRUE(imu.has_value()) << error;
  const auto other = writer.addTopic(
      ArtifactTopic{.name = "/other", .ontology_tag = "", .canonical_metadata = "", .timestamp_column = "timestamp_ns"},
      *scalarBatch(0, 1)->schema(), &error);
  ASSERT_TRUE(other.has_value()) << error;
  const auto object = writer.addObjectTopic(
      ArtifactTopic{
          .name = "/blob",
          .ontology_tag = "image",
          .canonical_metadata = R"({"builtin_object_type":"kImage"})",
          .timestamp_column = ""},
      &error);
  ASSERT_TRUE(object.has_value()) << error;

  // Deliberately decreasing log times make a reordering LogTimeOrder reader
  // observably violate the existing file-order replay contract.
  for (int i = 0; i < imu_batches; ++i) {
    const auto batch = scalarBatch(1000 + i * 10, 2);
    ASSERT_TRUE(writer.writeBatch(*imu, *batch, 1000 - i * 100, &error)) << error;
    if (i == 0) {
      ASSERT_TRUE(writer.writeBatch(*other, *scalarBatch(7000, 1), 7000, &error)) << error;
    }
  }
  for (int i = 0; i < object_samples; ++i) {
    const std::vector<std::uint8_t> payload(32 + i, static_cast<std::uint8_t>(0xA0 + i));
    ASSERT_TRUE(writer.writeObjectSample(*object, 500 - i * 10, payload.data(), payload.size(), &error)) << error;
  }
  ASSERT_TRUE(writer.close(&error)) << error;
}

void writeArtifactWithBadLaterBatch(const fs::path& path) {
  mosaico::ArtifactWriter writer;
  std::string error;
  ASSERT_TRUE(writer.open(path, R"({"kind":"incremental-error-test"})", &error)) << error;
  const auto channel = writer.addTopic(
      ArtifactTopic{.name = "/imu", .ontology_tag = "", .canonical_metadata = "", .timestamp_column = "timestamp_ns"},
      *scalarBatch(0, 1)->schema(), &error);
  ASSERT_TRUE(channel.has_value()) << error;
  ASSERT_TRUE(writer.writeBatch(*channel, *scalarBatch(1000, 2), 1000, &error)) << error;

  arrow::Int32Builder bad_builder;
  ASSERT_TRUE(bad_builder.Append(7).ok());
  std::shared_ptr<arrow::Array> bad_array;
  ASSERT_TRUE(bad_builder.Finish(&bad_array).ok());
  const auto bad_batch = arrow::RecordBatch::Make(
      arrow::schema({arrow::field("not_the_registered_schema", arrow::int32())}), 1, {bad_array});
  ASSERT_TRUE(writer.writeBatch(*channel, *bad_batch, 2000, &error)) << error;
  ASSERT_TRUE(writer.close(&error)) << error;
}

void overwriteLe64(const fs::path& path, std::uint64_t offset, std::uint64_t value) {
  unsigned char bytes[8];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<unsigned char>(value >> (8 * i));
  }
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.is_open());
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
  ASSERT_TRUE(static_cast<bool>(file));
}

bool readSummaryStart(const fs::path& path, std::uint64_t* summary_start) {
  constexpr std::streamoff kMcapTailBytes = 37;
  unsigned char footer[kMcapTailBytes] = {};
  std::ifstream file(path, std::ios::binary);
  file.seekg(-kMcapTailBytes, std::ios::end);
  file.read(reinterpret_cast<char*>(footer), sizeof(footer));
  if (!file) {
    return false;
  }
  *summary_start = 0;
  for (int i = 0; i < 8; ++i) {
    *summary_start |= static_cast<std::uint64_t>(footer[9 + i]) << (8 * i);
  }
  return true;
}

void overwriteByte(const fs::path& path, std::uint64_t offset, unsigned char value) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.is_open());
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char*>(&value), 1);
  ASSERT_TRUE(static_cast<bool>(file));
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
  std::vector<std::string> calls;
  std::uint64_t next_handle = 1;

  ReplaySinks sinks() {
    ReplaySinks s;
    s.append_scalar_stream = [this](
                                 const std::string& topic, ArrowArrayStream* stream,
                                 const std::string& timestamp_column, std::string* error) {
      calls.push_back("append:" + topic + ":" + timestamp_column);
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
      calls.push_back("register:" + topic + ":" + metadata);
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
  sinks.progress = [&ticks](std::uint64_t done, std::uint64_t total) {
    ticks.push_back({done, total});
    return done < 1;  // stop after the first delivered batch
  };
  std::string error;
  EXPECT_FALSE(mosaico::replayArtifact({scalarTopic("/a"), scalarTopic("/b")}, sinks, &error));
  EXPECT_NE(error.find("stopped"), std::string::npos);
  ASSERT_EQ(ticks.size(), 1u);
  EXPECT_TRUE(recorder.scalars.empty());  // the scalar sink observes its stream error
}

TEST(ArtifactReplay, MissingSinksFailClosed) {
  std::string error;
  EXPECT_FALSE(mosaico::replayArtifact({scalarTopic("/a")}, ReplaySinks{}, &error));
  EXPECT_NE(error.find("not fully configured"), std::string::npos);
}

TEST(ArtifactReplay, FileReplayDeliversFirstBatchBeforeLaterDecodeFailure) {
  TempRoot root("incremental");
  const fs::path path = root.path / "artifact.mcap";
  writeArtifactWithBadLaterBatch(path);

  Recorder recorder;
  ReplaySinks sinks = recorder.sinks();
  bool first_delivered = false;
  sinks.append_scalar_stream = [&](const std::string&, ArrowArrayStream* stream, const std::string&,
                                   std::string* sink_error) {
    auto reader = arrow::ImportRecordBatchReader(stream);
    if (!reader.ok()) {
      *sink_error = reader.status().ToString();
      return false;
    }
    auto first = (*reader)->Next();
    if (!first.ok() || *first == nullptr) {
      *sink_error = first.status().ToString();
      return false;
    }
    first_delivered = true;
    EXPECT_TRUE((*first)->Equals(*scalarBatch(1000, 2)));
    const auto bad = (*reader)->Next();
    EXPECT_FALSE(bad.ok());
    *sink_error = bad.status().ToString();
    return false;
  };

  std::string error;
  EXPECT_FALSE(mosaico::replayArtifact(path, sinks, &error));
  EXPECT_TRUE(first_delivered);
  EXPECT_NE(error.find("batch undecodable"), std::string::npos) << error;
}

TEST(ArtifactReplay, FileReplayCancellationStopsMidObjectTopicPromptly) {
  TempRoot root("cancel-object");
  const fs::path path = root.path / "artifact.mcap";
  writeStreamingArtifact(path, /*imu_batches=*/0, /*object_samples=*/50);

  Recorder recorder;
  ReplaySinks sinks = recorder.sinks();
  std::size_t pushed = 0;
  bool cancel = false;
  sinks.push_object = [&](std::uint64_t, std::int64_t, const std::uint8_t*, std::size_t, std::string*) {
    ++pushed;
    cancel = pushed == 3;
    return true;
  };
  sinks.is_cancelled = [&cancel]() { return cancel; };

  std::string error;
  EXPECT_FALSE(mosaico::replayArtifact(path, sinks, &error));
  EXPECT_EQ(error, "replay stopped");
  EXPECT_EQ(pushed, 3u);
}

TEST(ArtifactReplay, RawChunkSizeOverReplayCapFailsBeforePayloadDelivery) {
  TempRoot root("chunk-cap");
  const fs::path path = root.path / "artifact.mcap";
  mosaico::ArtifactWriter writer;
  std::string error;
  ASSERT_TRUE(writer.open(path, R"({"kind":"chunk-cap-test"})", &error)) << error;
  const auto object = writer.addObjectTopic(
      ArtifactTopic{
          .name = "/blob",
          .ontology_tag = "image",
          .canonical_metadata = R"({"builtin_object_type":"kImage"})",
          .timestamp_column = ""},
      &error);
  ASSERT_TRUE(object.has_value()) << error;
  const std::vector<std::uint8_t> payload(32, 0xAB);
  ASSERT_TRUE(writer.writeObjectSample(*object, 100, payload.data(), payload.size(), &error)) << error;
  ASSERT_TRUE(writer.close(&error)) << error;

  mcap::McapReader mcap_reader;
  ASSERT_TRUE(mcap_reader.open(path.string()).ok());
  ASSERT_TRUE(mcap_reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok());
  const auto chunks = mcap_reader.chunkIndexes();
  ASSERT_EQ(chunks.size(), 1u);
  const auto chunk_offset = chunks.front().chunkStartOffset;
  mcap_reader.close();
  // Chunk record framing is 9 bytes; uncompressedSize follows two uint64
  // timestamps in the body.
  overwriteLe64(path, chunk_offset + 9 + 16, (2ull << 30) + 1);

  Recorder recorder;
  ReplaySinks sinks = recorder.sinks();
  std::size_t pushed = 0;
  sinks.push_object = [&pushed](std::uint64_t, std::int64_t, const std::uint8_t*, std::size_t, std::string*) {
    ++pushed;
    return true;
  };
  EXPECT_FALSE(mosaico::replayArtifact(path, sinks, &error));
  EXPECT_EQ(pushed, 0u);
  EXPECT_NE(error.find("chunk preflight failed"), std::string::npos) << error;
  EXPECT_NE(error.find("exceeds the native replay cap"), std::string::npos) << error;
}

TEST(ArtifactReplay, ChunkRecordInSummaryFailsBeforeAnySinkCallback) {
  TempRoot root("summary-chunk");
  const fs::path path = root.path / "artifact.mcap";
  writeStreamingArtifact(path, /*imu_batches=*/1, /*object_samples=*/1);

  std::uint64_t summary_start = 0;
  ASSERT_TRUE(readSummaryStart(path, &summary_start));
  ASSERT_NE(summary_start, 0u);
  overwriteByte(path, summary_start, static_cast<unsigned char>(mcap::OpCode::Chunk));

  Recorder recorder;
  std::string error;
  EXPECT_FALSE(mosaico::preflightArtifactForReplay(path, &error));
  EXPECT_NE(error.find("Chunk record is forbidden in the summary section"), std::string::npos) << error;
  error.clear();
  EXPECT_FALSE(mosaico::replayArtifact(path, recorder.sinks(), &error));
  EXPECT_TRUE(recorder.calls.empty());
  EXPECT_NE(error.find("Chunk record is forbidden in the summary section"), std::string::npos) << error;
}

TEST(ArtifactReplay, OversizedRawHeaderFailsReplayPreflightWithoutAllocation) {
  TempRoot root("header-cap");
  const fs::path path = root.path / "artifact.mcap";
  writeStreamingArtifact(path, /*imu_batches=*/1, /*object_samples=*/1);

  // Eight magic bytes and one Header opcode precede the raw body length.
  overwriteLe64(path, /*offset=*/9, 16ull * 1024 * 1024 + 1);

  std::string error;
  EXPECT_FALSE(mosaico::preflightArtifactForReplay(path, &error));
  EXPECT_NE(error.find("Header body length is outside the bounded data section"), std::string::npos) << error;
}

TEST(ArtifactReplay, StreamingMultiTopicReplayMatchesMaterializedReaderByteForByte) {
  TempRoot root("multi-topic-equality");
  const fs::path path = root.path / "artifact.mcap";
  writeStreamingArtifact(path);

  std::string error;
  std::vector<ArtifactTopicData> expected;
  ASSERT_TRUE(mosaico::readArtifact(path, &expected, nullptr, &error)) << error;

  Recorder recorder;
  ReplaySinks sinks = recorder.sinks();
  std::size_t started_topics = 0;
  std::uint64_t started_messages = 0;
  std::vector<std::uint64_t> progress_ticks;
  sinks.start = [&](std::size_t topics, std::uint64_t messages, std::string*) {
    started_topics = topics;
    started_messages = messages;
    return true;
  };
  sinks.progress = [&](std::uint64_t done, std::uint64_t total) {
    EXPECT_EQ(total, 7u);
    progress_ticks.push_back(done);
    return true;
  };
  ASSERT_TRUE(mosaico::replayArtifact(path, sinks, &error)) << error;
  EXPECT_EQ(started_topics, 3u);
  EXPECT_EQ(started_messages, 7u);
  ASSERT_EQ(progress_ticks.size(), 7u);
  EXPECT_EQ(progress_ticks.back(), 7u);
  EXPECT_EQ(
      recorder.calls, (std::vector<std::string>{
                          "append:/imu:timestamp_ns", "append:/other:timestamp_ns",
                          R"(register:/blob:{"builtin_object_type":"kImage"})"}));
  ASSERT_EQ(expected.size(), 3u);
  for (const auto& topic : expected) {
    if (topic.is_object) {
      ASSERT_EQ(recorder.objects.at(topic.info.name).size(), topic.object_samples.size());
      for (std::size_t i = 0; i < topic.object_samples.size(); ++i) {
        EXPECT_EQ(recorder.objects.at(topic.info.name)[i].log_time_ns, topic.object_samples[i].log_time_ns);
        EXPECT_EQ(recorder.objects.at(topic.info.name)[i].payload, topic.object_samples[i].payload);
      }
    } else {
      const auto& actual = recorder.scalars.at(topic.info.name);
      EXPECT_EQ(actual.timestamp_column, topic.info.timestamp_column);
      ASSERT_EQ(actual.batches.size(), topic.batches.size());
      for (std::size_t i = 0; i < topic.batches.size(); ++i) {
        EXPECT_TRUE(actual.batches[i]->Equals(*topic.batches[i]));
      }
    }
  }
}

TEST(ArtifactReplay, ReplayStartFailurePreservesHostErrorAndIsNotCancellation) {
  Recorder recorder;
  ReplaySinks sinks = recorder.sinks();
  sinks.start = [](std::size_t, std::uint64_t, std::string* start_error) {
    *start_error = "progress surface unavailable";
    return false;
  };

  std::string error;
  EXPECT_FALSE(mosaico::replayArtifact({scalarTopic("/imu")}, sinks, &error));
  EXPECT_EQ(error, "replay start failed: progress surface unavailable");
  EXPECT_FALSE(mosaico::isReplayCancellation(error, /*stop_requested=*/false));
  EXPECT_TRUE(mosaico::isReplayCancellation("replay stopped", /*stop_requested=*/false));
  EXPECT_TRUE(mosaico::isReplayCancellation(error, /*stop_requested=*/true));
}
