// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The pj.descriptor_import.v1 provider's NETWORK-FREE surfaces: the query
// taxonomy (malformed / env-untrusted / env-allowlisted / materialized-with-pin) and
// startImport's fail-closed rejections (flags, callbacks, descriptor,
// unbound host). The full job path needs a live Flight server and is covered
// by the scripted E2E (scripts/e2e-mosaico-layout-import.sh). Hermetic via
// MOSAICO_TRUSTED_ORIGINS + MOSAICO_CACHE_DIR (POSIX-only, like the
// connector env tests).
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <pj_base/sdk/descriptor_import/provider_job.hpp>
#include <pj_base/sdk/settings_store_host.hpp>
#include <string>

#include "descriptor_import/artifact_capture.hpp"
#include "descriptor_import/source_descriptor.hpp"
#include "descriptor_import_provider.hpp"
#include "source_presentation.hpp"

#if !defined(_WIN32)

namespace mosaico::testing {

class DescriptorImportProviderTestAccess {
 public:
  static bool claimTransferWatchdogExpiry(std::atomic<bool>& transfer_in_progress) {
    return DescriptorImportProvider::claimTransferWatchdogExpiry(transfer_in_progress);
  }
};

}  // namespace mosaico::testing

namespace {

namespace fs = std::filesystem;
using mosaico::DescriptorImportProvider;
using mosaico::SourceDescriptor;

// mkdtemp-based cache root + the environment the provider resolves.
struct ProviderEnv {
  fs::path cache_dir;

  ProviderEnv() {
    std::string cache_template = (fs::temp_directory_path() / "mosaico-di-cache-XXXXXX").string();
    cache_dir = ::mkdtemp(cache_template.data());
    ::unsetenv("MOSAICO_TRUSTED_ORIGINS");
    ::setenv("MOSAICO_CACHE_DIR", cache_dir.string().c_str(), 1);
  }
  ~ProviderEnv() {
    ::unsetenv("MOSAICO_TRUSTED_ORIGINS");
    ::unsetenv("MOSAICO_CACHE_DIR");
    std::error_code ec;
    fs::remove_all(cache_dir, ec);
  }

  void setTrustAllowlist(const char* origins) {
    ::setenv("MOSAICO_TRUSTED_ORIGINS", origins, 1);
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

// Materialize a valid artifact for `d` through the real capture (one scalar
// batch), releasing the finalize-time lease so the query's own pin is what
// keeps it alive.
void materializeArtifact(const SourceDescriptor& d) {
  mosaico::ArtifactCapture capture(d);
  ASSERT_TRUE(capture.armed()) << capture.disarmReason();
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
  capture.captureScalarTopic(
      "/imu", *schema, "timestamp_ns", {arrow::RecordBatch::Make(schema, 1, {t_array, v_array})});
  auto finalized = capture.finish(/*complete=*/true);
  ASSERT_TRUE(finalized.has_value()) << capture.disarmReason();
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

TEST(DescriptorImportQuery, UntrustedThenEnvAllowlistedThenMaterialized) {
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
  EXPECT_EQ(
      str(result.message), "server not trusted on this machine; confirm the download or set MOSAICO_TRUSTED_ORIGINS");
  EXPECT_EQ(result.estimated_bytes, 0u);

  // 2. An exact strict origin in the process allowlist flips the verdict.
  env.setTrustAllowlist("https://ignored:443, grpc+tls://other.example:6726, GRPC+TLS://DEMO.MOSAICO.DEV:6726");
  result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
  EXPECT_EQ(result.trust, PJ_DESCRIPTOR_TRUST_TRUSTED);
  EXPECT_EQ(result.is_materialized, 0u);
  EXPECT_TRUE(str(result.message).empty());

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
  mosaico::ArtifactCapture contender(d);
  EXPECT_FALSE(contender.armed());

  // 4. Repeat queries stay stable (the pin is idempotent, not stacking).
  result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
  EXPECT_EQ(result.is_materialized, 1u);
}

TEST(DescriptorImportQuery, TrustAllowlistFailsClosedForDifferentOrMalformedOrigins) {
  ProviderEnv env;
  DescriptorImportProvider provider;
  const std::string json = toSourceDescriptorJson(descriptor());

  for (const char* allowlist : {
           "",
           "not-an-origin,https://demo.mosaico.dev:6726",
           "grpc://demo.mosaico.dev:6726",
           "grpc+tls://demo.mosaico.dev:6727",
           "grpc+tls://other.mosaico.dev:6726",
       }) {
    env.setTrustAllowlist(allowlist);
    auto result = freshResult();
    ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
    EXPECT_EQ(result.trust, PJ_DESCRIPTOR_TRUST_NEEDS_CONFIRMATION) << allowlist;
  }
}

TEST(DescriptorImportQuery, LockContentionIsAMissWithRetryHint) {
  ProviderEnv env;
  env.setTrustAllowlist("grpc+tls://demo.mosaico.dev:6726");
  DescriptorImportProvider provider;
  SourceDescriptor d = descriptor();
  d.sequence = "contention";
  const std::string json = toSourceDescriptorJson(d);
  materializeArtifact(d);

  auto cache = mosaico::makeArtifactCache(env.cache_dir);
  auto writer = cache.beginWrite(mosaico::descriptorIdentity(d));
  ASSERT_TRUE(writer) << writer.error().message;

  auto result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
  EXPECT_EQ(result.trust, PJ_DESCRIPTOR_TRUST_TRUSTED);
  EXPECT_EQ(result.is_materialized, 0u);
  EXPECT_EQ(result.estimated_bytes, 0u);
  EXPECT_NE(str(result.message).find("retry"), std::string::npos) << str(result.message);
  EXPECT_TRUE(fs::is_regular_file(fs::path(str(result.local_path_utf8))));
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

  auto short_callbacks = callbacks;
  short_callbacks.struct_size = offsetof(PJ_descriptor_import_callbacks_v1_t, on_terminal);
  EXPECT_FALSE(provider.startImport(&request, &short_callbacks, nullptr, &job, nullptr));

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

TEST(DescriptorImportJob, WatchdogExpiryAfterTransferIsInert) {
  std::atomic<bool> transfer_in_progress{false};
  std::atomic<bool> expired{false};
  std::atomic<PJ_descriptor_import_outcome_t> terminal{PJ_DESCRIPTOR_IMPORT_FAILED};
  std::mutex callback_mu;
  std::condition_variable callback_cv;
  bool callback_ran = false;

  PJ_descriptor_import_callbacks_v1_t callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.on_terminal = [](void* ctx, PJ_descriptor_import_outcome_t outcome, PJ_string_view_t) noexcept {
    static_cast<std::atomic<PJ_descriptor_import_outcome_t>*>(ctx)->store(outcome);
  };

  PJ_joinable_job_t job{};
  ASSERT_TRUE(
      PJ::sdk::descriptor_import::ProviderJob::start(
          [&](PJ::sdk::descriptor_import::JobControl& control) {
            transfer_in_progress.store(true);
            control.armWatchdog(std::chrono::milliseconds(5), [&] {
              if (mosaico::testing::DescriptorImportProviderTestAccess::claimTransferWatchdogExpiry(
                      transfer_in_progress)) {
                expired.store(true);
              }
              {
                std::lock_guard<std::mutex> lock(callback_mu);
                callback_ran = true;
              }
              callback_cv.notify_one();
            });
            transfer_in_progress.store(false);  // pull returned
            std::unique_lock<std::mutex> lock(callback_mu);
            (void)callback_cv.wait_for(lock, std::chrono::seconds(1), [&] { return callback_ran; });
            return PJ::sdk::descriptor_import::ImportOutcome{
                PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY, "transfer completed"};
          },
          &callbacks, &terminal, &job, nullptr));

  ASSERT_NE(job.vtable, nullptr);
  job.vtable->join(job.ctx);
  EXPECT_TRUE(callback_ran);
  EXPECT_FALSE(expired.load());
  EXPECT_EQ(terminal.load(), PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY);
  job.vtable->destroy(job.ctx);
}

}  // namespace

#endif  // !_WIN32
