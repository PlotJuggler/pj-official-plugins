// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The Arrow-in-MCAP cache artifact: write -> validate -> read round trip
// (multi-topic, heterogeneous schemas, verbatim batches), the validator's
// rejection matrix (identity mismatch, foreign junk, truncation, content
// mismatch, forged summary span), and the artifact <-> SessionFileCache
// integration through the real lock -> partial -> finalize path.
#include "arrow_cache_artifact.hpp"

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "core/sha256.h"
#include "source_descriptor.hpp"
#include "test_support_fs.hpp"

namespace {

namespace fs = std::filesystem;
using mosaico::ArtifactTopic;
using mosaico::ArtifactTopicData;
using mosaico::ArtifactWriter;
using mosaico::SessionFileCache;

struct TempRoot : mosaico_test::ScopedTempDir {
  explicit TempRoot(const std::string& name) : ScopedTempDir("mosaico-artifact-test-" + name) {}
};

mosaico::SourceDescriptor descriptor() {
  mosaico::SourceDescriptor d;
  d.version = 1;
  d.kind = "mosaico-sequence";
  d.server_uri = "grpc+tls://demo.mosaico.dev:6726";
  d.sequence = "seq_a";
  d.topics = {"/imu", "/blob"};
  d.start_ns = 0;
  d.end_ns = 0;
  d.display_name = "artifact-test";
  return d;
}

// The identity's digest component for `d` (what the store hands validators).
std::string hexFor(const mosaico::SourceDescriptor& d) {
  return mosaico::sha256HexPrefix(mosaico::canonicalSourceDescriptorJson(d), 16);
}

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

std::shared_ptr<arrow::RecordBatch> blobBatch(int rows) {
  arrow::BinaryBuilder blob_builder;
  for (int i = 0; i < rows; ++i) {
    EXPECT_TRUE(blob_builder.Append(std::string(64, static_cast<char>('a' + i))).ok());
  }
  std::shared_ptr<arrow::Array> blob_array;
  EXPECT_TRUE(blob_builder.Finish(&blob_array).ok());
  const auto schema = arrow::schema({arrow::field("payload", arrow::binary())});
  return arrow::RecordBatch::Make(schema, rows, {blob_array});
}

// Write a complete two-topic artifact at `path`; returns the producer counts.
SessionFileCache::ExpectedContent writeArtifact(const fs::path& path, const mosaico::SourceDescriptor& d) {
  ArtifactWriter writer;
  std::string error;
  EXPECT_TRUE(writer.open(path, mosaico::canonicalSourceDescriptorJson(d), &error)) << error;
  const auto imu = writer.addTopic(
      ArtifactTopic{.name = "/imu", .ontology_tag = "", .canonical_metadata = "", .timestamp_column = "timestamp_ns"},
      *scalarBatch(0, 1)->schema(), &error);
  EXPECT_TRUE(imu.has_value()) << error;
  const auto blob = writer.addTopic(
      ArtifactTopic{
          .name = "/blob",
          .ontology_tag = "image",
          .canonical_metadata = R"({"builtin_object_type":"kImage"})",
          .timestamp_column = ""},
      *blobBatch(1)->schema(), &error);
  EXPECT_TRUE(blob.has_value()) << error;
  EXPECT_TRUE(writer.writeBatch(*imu, *scalarBatch(100, 3), 100, &error)) << error;
  EXPECT_TRUE(writer.writeBatch(*imu, *scalarBatch(200, 2), 200, &error)) << error;
  EXPECT_TRUE(writer.writeBatch(*blob, *blobBatch(2), 150, &error)) << error;
  SessionFileCache::ExpectedContent expected;
  EXPECT_TRUE(writer.close(&expected, &error)) << error;
  return expected;
}

}  // namespace

TEST(ArrowCacheArtifact, WriteValidateReadRoundTrip) {
  TempRoot root("roundtrip");
  const fs::path path = root.path / "artifact.mcap";
  const auto d = descriptor();
  const auto expected = writeArtifact(path, d);
  EXPECT_EQ(expected.row_count, 7u);  // 3 + 2 + 2
  EXPECT_EQ(expected.topic_count, 2u);

  std::string error;
  // Finalize-style validation (with the completeness pin) and lookup-style.
  EXPECT_TRUE(mosaico::validateArtifact(path, hexFor(d), expected, &error)) << error;
  EXPECT_TRUE(mosaico::validateArtifact(path, hexFor(d), std::nullopt, &error)) << error;

  std::vector<ArtifactTopicData> topics;
  std::string canonical;
  ASSERT_TRUE(mosaico::readArtifact(path, &topics, &canonical, &error)) << error;
  EXPECT_EQ(canonical, mosaico::canonicalSourceDescriptorJson(d));
  ASSERT_EQ(topics.size(), 2u);

  const ArtifactTopicData& imu = topics[0];
  EXPECT_EQ(imu.info.name, "/imu");
  EXPECT_EQ(imu.info.timestamp_column, "timestamp_ns");
  EXPECT_TRUE(imu.info.ontology_tag.empty());
  ASSERT_EQ(imu.batches.size(), 2u);
  EXPECT_TRUE(imu.batches[0]->Equals(*scalarBatch(100, 3)));
  EXPECT_TRUE(imu.batches[1]->Equals(*scalarBatch(200, 2)));
  EXPECT_EQ(imu.batch_log_times_ns, (std::vector<std::int64_t>{100, 200}));

  const ArtifactTopicData& blob = topics[1];
  EXPECT_EQ(blob.info.name, "/blob");
  EXPECT_EQ(blob.info.ontology_tag, "image");
  EXPECT_EQ(blob.info.canonical_metadata, R"({"builtin_object_type":"kImage"})");
  ASSERT_EQ(blob.batches.size(), 1u);
  EXPECT_TRUE(blob.batches[0]->Equals(*blobBatch(2)));
}

TEST(ArrowCacheArtifact, ValidatorRejectsWrongIdentity) {
  TempRoot root("wrong-id");
  const fs::path path = root.path / "artifact.mcap";
  (void)writeArtifact(path, descriptor());
  std::string error;
  EXPECT_FALSE(mosaico::validateArtifact(path, std::string(32, 'f'), std::nullopt, &error));
  EXPECT_NE(error.find("identity mismatch"), std::string::npos) << error;
}

TEST(ArrowCacheArtifact, ValidatorRejectsForeignJunkAndTruncation) {
  TempRoot root("junk");
  const fs::path junk = root.path / "junk.mcap";
  {
    std::ofstream out(junk, std::ios::binary);
    out << "this is not an mcap file at all";
  }
  std::string error;
  EXPECT_FALSE(mosaico::validateArtifact(junk, std::string(32, 'a'), std::nullopt, &error));

  const fs::path truncated = root.path / "truncated.mcap";
  const auto d = descriptor();
  (void)writeArtifact(truncated, d);
  fs::resize_file(truncated, fs::file_size(truncated) / 2);
  EXPECT_FALSE(mosaico::validateArtifact(truncated, hexFor(d), std::nullopt, &error));
}

TEST(ArrowCacheArtifact, ValidatorRejectsContentMismatchButLookupStillPasses) {
  TempRoot root("content");
  const fs::path path = root.path / "artifact.mcap";
  const auto d = descriptor();
  auto expected = writeArtifact(path, d);
  std::string error;
  expected.row_count += 1;  // a cleanly-closed PREFIX would look like this
  EXPECT_FALSE(mosaico::validateArtifact(path, hexFor(d), expected, &error));
  EXPECT_NE(error.find("content summary mismatch"), std::string::npos) << error;
  // The lookup path (no pin) still accepts the structurally valid file.
  EXPECT_TRUE(mosaico::validateArtifact(path, hexFor(d), std::nullopt, &error)) << error;
}

TEST(ArrowCacheArtifact, AbortedWriterLeavesUnvalidatablePartial) {
  TempRoot root("abort");
  const fs::path path = root.path / "partial.mcap";
  ArtifactWriter writer;
  std::string error;
  ASSERT_TRUE(writer.open(path, mosaico::canonicalSourceDescriptorJson(descriptor()), &error)) << error;
  const auto imu = writer.addTopic(
      ArtifactTopic{.name = "/imu", .ontology_tag = "", .canonical_metadata = "", .timestamp_column = "timestamp_ns"},
      *scalarBatch(0, 1)->schema(), &error);
  ASSERT_TRUE(imu.has_value()) << error;
  ASSERT_TRUE(writer.writeBatch(*imu, *scalarBatch(100, 3), 100, &error)) << error;
  writer.abort();
  EXPECT_FALSE(mosaico::validateArtifact(path, hexFor(descriptor()), std::nullopt, &error));
}

// A forged file whose footer points at a multi-gigabyte summary span must be
// refused by the raw-footer preflight BEFORE the MCAP summary parser
// allocates anything. (Sparse file: large st_size, tiny disk use.)
TEST(ArrowCacheArtifact, ForgedOversizedSummarySpanIsRefusedBeforeParsing) {
  TempRoot root("forged");
  const fs::path path = root.path / "forged.mcap";
  {
    std::ofstream out(path, std::ios::binary);
    out.seekp(20ll * 1024 * 1024);  // sparse body
    unsigned char tail[37] = {};
    tail[0] = 0x02;  // Footer opcode
    tail[9] = 0x01;  // summary_start = 1 -> span ~= 20 MiB > budget
    out.write(reinterpret_cast<const char*>(tail), sizeof(tail));
  }
  std::string error;
  EXPECT_FALSE(mosaico::validateArtifact(path, std::string(32, 'a'), std::nullopt, &error));
  EXPECT_NE(error.find("bounded query budget"), std::string::npos) << error;
}

// End-to-end with the store: the artifact validator is exactly the store's
// injected Validator, driven through the real lock -> partial -> finalize
// path with the writer-reported producer counts.
TEST(ArrowCacheArtifact, CacheRoundTripThroughStore) {
  TempRoot root("store");
  SessionFileCache cache(root.path, mosaico::validateArtifact);
  const auto d = descriptor();
  const std::string identity = mosaico::descriptorIdentity(d);
  std::string error;
  auto lock = cache.tryLockForMaterialize(identity, &error);
  ASSERT_TRUE(lock.has_value()) << error;
  const auto expected = writeArtifact(cache.partialPathFor(*lock), d);
  ASSERT_TRUE(cache.finalize(*lock, expected, &error)) << error;

  fs::path hit;
  EXPECT_TRUE(cache.lookup(identity, &hit));
  std::vector<ArtifactTopicData> topics;
  ASSERT_TRUE(mosaico::readArtifact(hit, &topics, nullptr, &error)) << error;
  EXPECT_EQ(topics.size(), 2u);
}
