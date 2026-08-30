// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The pj.descriptor_import.v1 provider's NETWORK-FREE surfaces: the query
// taxonomy (malformed / untrusted-miss / trusted / materialized-with-pin) and
// startImport's fail-closed rejections (flags, callbacks, descriptor,
// unbound host). The full job path needs a live Flight server and is covered
// by the scripted E2E (scripts/e2e-mosaico-layout-import.sh). Hermetic via
// MOSAICO_CONFIG_DIR + MOSAICO_CACHE_DIR (POSIX-only, like the ingest-core
// env tests).
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <pj_base/sdk/settings_store_host.hpp>
#include <string>

#include "cache_tee_session.hpp"
#include "descriptor_import_provider.hpp"
#include "source_descriptor.hpp"
#include "source_presentation.hpp"

#if !defined(_WIN32)

namespace {

namespace fs = std::filesystem;
using mosaico::DescriptorImportProvider;
using mosaico::SourceDescriptor;

// mkdtemp-based temp roots + the env the provider resolves at construction.
struct ProviderEnv {
  fs::path config_dir;
  fs::path cache_dir;

  ProviderEnv() {
    std::string config_template = (fs::temp_directory_path() / "mosaico-di-config-XXXXXX").string();
    std::string cache_template = (fs::temp_directory_path() / "mosaico-di-cache-XXXXXX").string();
    config_dir = ::mkdtemp(config_template.data());
    cache_dir = ::mkdtemp(cache_template.data());
    ::setenv("MOSAICO_CONFIG_DIR", config_dir.string().c_str(), 1);
    ::setenv("MOSAICO_CACHE_DIR", cache_dir.string().c_str(), 1);
  }
  ~ProviderEnv() {
    ::unsetenv("MOSAICO_CONFIG_DIR");
    ::unsetenv("MOSAICO_CACHE_DIR");
    std::error_code ec;
    fs::remove_all(config_dir, ec);
    fs::remove_all(cache_dir, ec);
  }
};

SourceDescriptor descriptor() {
  SourceDescriptor d;
  d.version = 1;
  d.kind = "mosaico-sequence";
  d.server_uri = "grpc+tls://demo.mosaico.dev:6726";
  d.sequence = "seq_a";
  d.topics = {"/imu"};
  d.start_ns = 0;
  d.end_ns = 0;
  d.display_name = "contract-test";
  return d;
}

PJ_string_view_t view(const std::string& text) {
  return PJ_string_view_t{text.data(), text.size()};
}

PJ_descriptor_query_result_v1_t freshResult() {
  PJ_descriptor_query_result_v1_t result{};
  result.struct_size = sizeof(result);
  return result;
}

std::string str(PJ_string_view_t sv) {
  return std::string(sv.data == nullptr ? "" : sv.data, sv.data == nullptr ? 0 : sv.size);
}

// Materialize a valid artifact for `d` through the real tee (one scalar
// batch), releasing the finalize-time lease so the query's own pin is what
// keeps it alive.
void materializeArtifact(const SourceDescriptor& d) {
  mosaico::CacheTeeSession tee(d);
  ASSERT_TRUE(tee.armed()) << tee.disarmReason();
  arrow::Int64Builder t_builder;
  arrow::DoubleBuilder v_builder;
  ASSERT_TRUE(t_builder.Append(100).ok());
  ASSERT_TRUE(v_builder.Append(1.0).ok());
  std::shared_ptr<arrow::Array> t_array;
  std::shared_ptr<arrow::Array> v_array;
  ASSERT_TRUE(t_builder.Finish(&t_array).ok());
  ASSERT_TRUE(v_builder.Finish(&v_array).ok());
  const auto schema =
      arrow::schema({arrow::field("timestamp_ns", arrow::int64()), arrow::field("value", arrow::float64())});
  tee.teeScalarTopic("/imu", *schema, "timestamp_ns", {arrow::RecordBatch::Make(schema, 1, {t_array, v_array})});
  auto finalized = tee.finish(/*complete=*/true);
  ASSERT_TRUE(finalized.has_value()) << tee.disarmReason();
}

TEST(DescriptorImportQuery, MalformedDescriptorIsAContractFailure) {
  ProviderEnv env;
  DescriptorImportProvider provider;
  auto result = freshResult();
  PJ_error_t error{};
  const std::string bad = "{not json";
  EXPECT_FALSE(provider.queryDescriptor(view(bad), &result, &error));
}

TEST(DescriptorImportQuery, PublishesPresentationThroughHostSettings) {
  ProviderEnv env;
  PJ::sdk::InMemorySettingsBackend settings_backend;
  PJ::sdk::SettingsStoreHost settings_host(settings_backend);
  DescriptorImportProvider provider;
  provider.bind(PJ::sdk::SettingsView{settings_host.view()}, {});
  const SourceDescriptor d = descriptor();
  const std::string json = toSourceDescriptorJson(d);

  auto result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));

  const std::string group = mosaico::sourcePresentationSettingsGroup(mosaico::descriptorIdentity(d));
  EXPECT_EQ(settings_backend.getString(group + "/display_name"), std::optional<std::string>("contract-test"));
  EXPECT_EQ(settings_backend.getString(group + "/origin"), std::optional<std::string>("demo.mosaico.dev:6726"));
}

TEST(DescriptorImportQuery, UntrustedMissThenTrustedThenMaterialized) {
  ProviderEnv env;
  DescriptorImportProvider provider;
  const SourceDescriptor d = descriptor();
  const std::string json = toSourceDescriptorJson(d);

  // 1. Valid + untrusted + cache miss.
  auto result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
  EXPECT_EQ(result.trust, PJ_DESCRIPTOR_TRUST_NEEDS_CONFIRMATION);
  EXPECT_EQ(result.is_materialized, 0u);
  EXPECT_EQ(str(result.source_identity), mosaico::descriptorIdentity(d));
  EXPECT_FALSE(str(result.local_path_utf8).empty());
  EXPECT_FALSE(str(result.message).empty());
  EXPECT_EQ(result.estimated_bytes, 0u);

  // 2. A successful interactive connect flips the verdict (write-through:
  // the durable ledger AND the in-memory set the query consults).
  ASSERT_TRUE(provider.recordSuccessfulConnect(d.server_uri));
  result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
  EXPECT_EQ(result.trust, PJ_DESCRIPTOR_TRUST_TRUSTED);
  EXPECT_EQ(result.is_materialized, 0u);

  // 3. A materialized artifact classifies as a hit with a size estimate...
  materializeArtifact(d);
  result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
  EXPECT_EQ(result.trust, PJ_DESCRIPTOR_TRUST_TRUSTED);
  EXPECT_EQ(result.is_materialized, 1u);
  EXPECT_GT(result.estimated_bytes, 0u);
  EXPECT_TRUE(fs::is_regular_file(fs::path(str(result.local_path_utf8))));

  // ...and the query PINNED it: a re-materialization attempt of the same
  // identity must now fail to arm (the shared read lease blocks the
  // exclusive materialize lock).
  mosaico::CacheTeeSession contender(d);
  EXPECT_FALSE(contender.armed());

  // 4. Repeat queries stay stable (the pin is idempotent, not stacking).
  result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
  EXPECT_EQ(result.is_materialized, 1u);
}

TEST(DescriptorImportStart, FailClosedRejections) {
  ProviderEnv env;
  DescriptorImportProvider provider;
  const std::string json = toSourceDescriptorJson(descriptor());

  PJ_descriptor_import_callbacks_v1_t callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.on_terminal = [](void*, PJ_descriptor_import_outcome_t, PJ_string_view_t) noexcept {};

  PJ_descriptor_import_start_request_v1_t request{};
  request.struct_size = sizeof(request);
  request.descriptor_json = view(json);

  // out_job must stay UNTOUCHED on every rejection.
  PJ_joinable_job_t job{};
  job.ctx = reinterpret_cast<void*>(0x1234);

  // Null request / null out_job.
  EXPECT_FALSE(provider.startImport(nullptr, &callbacks, nullptr, &job, nullptr));
  EXPECT_FALSE(provider.startImport(&request, &callbacks, nullptr, nullptr, nullptr));

  // Unknown flag bits fail closed FIRST.
  auto flagged = request;
  flagged.flags = UINT64_C(1) << 42;
  EXPECT_FALSE(provider.startImport(&flagged, &callbacks, nullptr, &job, nullptr));

  // on_terminal is required.
  auto no_terminal = callbacks;
  no_terminal.on_terminal = nullptr;
  EXPECT_FALSE(provider.startImport(&request, &no_terminal, nullptr, &job, nullptr));
  EXPECT_FALSE(provider.startImport(&request, nullptr, nullptr, &job, nullptr));

  // Malformed descriptor.
  auto bad_request = request;
  const std::string bad = "{}";
  bad_request.descriptor_json = view(bad);
  EXPECT_FALSE(provider.startImport(&bad_request, &callbacks, nullptr, &job, nullptr));

  // Valid request against an UNBOUND provider (no host bindings).
  EXPECT_FALSE(provider.startImport(&request, &callbacks, nullptr, &job, nullptr));

  // No rejection touched out_job.
  EXPECT_EQ(job.ctx, reinterpret_cast<void*>(0x1234));
}

}  // namespace

#endif  // !_WIN32
