// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The per-Download cache tee: arm -> tee scalar batches + object blobs ->
// finish(complete) produces a validated, readable artifact under a read
// lease; every failure mode (bad descriptor, lock contention, incomplete
// fetch) lands eager-only with no partial left behind. Hermetic via
// MOSAICO_CACHE_DIR (POSIX-only, like the store's env test).
#include "cache_tee_session.hpp"

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "artifact_replay.hpp"
#include "core/sha256.h"
#include "test_support_fs.hpp"

#if !defined(_WIN32)

namespace {

namespace fs = std::filesystem;
using mosaico::CacheTeeSession;
using mosaico::SourceDescriptor;

struct CacheEnv : mosaico_test::ScopedTempDir {
  explicit CacheEnv(const std::string& name) : ScopedTempDir("mosaico-tee-test-" + name) {
    ::setenv("MOSAICO_CACHE_DIR", path.string().c_str(), 1);
  }
  ~CacheEnv() {
    ::unsetenv("MOSAICO_CACHE_DIR");
  }
};

SourceDescriptor descriptor() {
  SourceDescriptor d;
  d.version = 1;
  d.kind = "mosaico-sequence";
  d.server_uri = "grpc+tls://demo.mosaico.dev:6726";
  d.sequence = "seq_a";
  d.topics = {"/imu", "/blob"};
  d.start_ns = 0;
  d.end_ns = 0;
  d.display_name = "tee-test";
  return d;
}

std::shared_ptr<arrow::RecordBatch> scalarBatch(std::int64_t t0, int rows) {
  arrow::Int64Builder t_builder;
  arrow::DoubleBuilder v_builder;
  for (int i = 0; i < rows; ++i) {
    EXPECT_TRUE(t_builder.Append(t0 + i).ok());
    EXPECT_TRUE(v_builder.Append(static_cast<double>(i)).ok());
  }
  std::shared_ptr<arrow::Array> t_array;
  std::shared_ptr<arrow::Array> v_array;
  EXPECT_TRUE(t_builder.Finish(&t_array).ok());
  EXPECT_TRUE(v_builder.Finish(&v_array).ok());
  const auto schema =
      arrow::schema({arrow::field("timestamp_ns", arrow::int64()), arrow::field("value", arrow::float64())});
  return arrow::RecordBatch::Make(schema, rows, {t_array, v_array});
}

bool dirHasPartial(const fs::path& root) {
  for (const auto& entry : fs::directory_iterator(root)) {
    if (entry.path().filename().string().find(".partial.") != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(CacheTeeSession, CompleteFetchProducesValidatedReadableArtifact) {
  CacheEnv env("complete");
  const auto d = descriptor();
  CacheTeeSession tee(d);
  ASSERT_TRUE(tee.armed()) << tee.disarmReason();
  EXPECT_EQ(tee.identity(), mosaico::descriptorIdentity(d));

  const auto batch = scalarBatch(100, 3);
  tee.teeScalarTopic("/imu", *batch->schema(), "timestamp_ns", {batch, scalarBatch(200, 2)});
  const auto tap = tee.objectTee("/blob", "image", R"({"builtin_object_type":"kImage"})");
  const std::vector<std::uint8_t> blob(32, 0x5A);
  tap(150, blob.data(), blob.size());
  tap(250, blob.data(), blob.size());
  ASSERT_TRUE(tee.armed()) << tee.disarmReason();

  const auto finalized = tee.finish(/*complete=*/true);
  ASSERT_TRUE(finalized.has_value()) << tee.disarmReason();
  EXPECT_TRUE(fs::is_regular_file(finalized->path));
  EXPECT_TRUE(finalized->lease.has_value());
  EXPECT_FALSE(dirHasPartial(env.path));

  // The artifact validates against the identity and reads back exactly.
  const std::string hex = mosaico::sha256HexPrefix(mosaico::canonicalSourceDescriptorJson(d), 16);
  std::string error;
  EXPECT_TRUE(mosaico::validateArtifact(finalized->path, hex, std::nullopt, &error)) << error;
  std::vector<mosaico::ArtifactTopicData> topics;
  std::string canonical;
  ASSERT_TRUE(mosaico::readArtifact(finalized->path, &topics, &canonical, &error)) << error;
  EXPECT_EQ(canonical, mosaico::canonicalSourceDescriptorJson(d));
  ASSERT_EQ(topics.size(), 2u);
  EXPECT_FALSE(topics[0].is_object);
  ASSERT_EQ(topics[0].batches.size(), 2u);
  EXPECT_TRUE(topics[0].batches[0]->Equals(*scalarBatch(100, 3)));
  EXPECT_TRUE(topics[1].is_object);
  ASSERT_EQ(topics[1].object_samples.size(), 2u);
  EXPECT_EQ(topics[1].object_samples[0].log_time_ns, 150);
  EXPECT_EQ(topics[1].object_samples[0].payload, blob);

  // The dataset-lifetime lease blocks a re-materialization until released.
  {
    CacheTeeSession contender(d);
    EXPECT_FALSE(contender.armed());
  }
}

TEST(CacheTeeSession, IncompleteFetchLeavesNothingBehind) {
  CacheEnv env("incomplete");
  CacheTeeSession tee(descriptor());
  ASSERT_TRUE(tee.armed()) << tee.disarmReason();
  const auto batch = scalarBatch(100, 3);
  tee.teeScalarTopic("/imu", *batch->schema(), "timestamp_ns", {batch});
  EXPECT_FALSE(tee.finish(/*complete=*/false).has_value());
  EXPECT_FALSE(dirHasPartial(env.path));
  bool any_artifact = false;
  for (const auto& entry : fs::directory_iterator(env.path)) {
    if (entry.path().extension() == ".pjmosaico") {
      any_artifact = true;
    }
  }
  EXPECT_FALSE(any_artifact);
}

TEST(CacheTeeSession, UncacheableDescriptorDisarmsInsteadOfFailing) {
  CacheEnv env("bad-descriptor");
  SourceDescriptor d = descriptor();
  d.server_uri = "demo.mosaico.dev:6726";  // scheme-less: valid to connect, not to embed
  CacheTeeSession tee(d);
  EXPECT_FALSE(tee.armed());
  EXPECT_NE(tee.disarmReason().find("not cacheable"), std::string::npos) << tee.disarmReason();
  // Taps are inert, never crashing.
  const auto tap = tee.objectTee("/blob", "image", "{}");
  const std::vector<std::uint8_t> blob(4, 1);
  tap(1, blob.data(), blob.size());
  EXPECT_FALSE(tee.finish(true).has_value());
}

TEST(CacheTeeSession, AbandonedSessionCleansItsPartial) {
  CacheEnv env("abandoned");
  {
    CacheTeeSession tee(descriptor());
    ASSERT_TRUE(tee.armed()) << tee.disarmReason();
    // Destroyed without finish(): the destructor aborts and cleans up.
  }
  EXPECT_FALSE(dirHasPartial(env.path));
}

#endif  // !_WIN32
