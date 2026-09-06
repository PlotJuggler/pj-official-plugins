// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// The pj.descriptor_import.v1 provider's NETWORK-FREE surfaces: the query
// taxonomy (malformed / env-untrusted / env-allowlisted), startImport's
// fail-closed rejections (flags, callbacks, descriptor, unbound host), and
// the transfer watchdog's post-completion inertness. The full job path needs
// a live Flight server and is covered by the scripted E2E
// (scripts/e2e-layout-import.sh). Hermetic via MOSAICO_TRUSTED_ORIGINS
// (POSIX-only, like the other env-driven tests).
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <pj_base/sdk/settings_store_host.hpp>
#include <pj_base/sdk/source/presentation.hpp>
#include <pj_base/sdk/source/provider_job.hpp>
#include <string>

#include "credential_resolve.hpp"
#include "descriptor_import_provider.hpp"
#include "fetch_worker.hpp"
#include "source_descriptor.hpp"
#include "worker_types.h"

#if !defined(_WIN32)

namespace mosaico::testing {

class DescriptorImportProviderTestAccess {
 public:
  static bool claimTransferWatchdogExpiry(std::atomic<bool>& transfer_in_progress) {
    return DescriptorImportProvider::claimTransferWatchdogExpiry(transfer_in_progress);
  }
};

// This binary's slice of the FetchWorker test surface (the full one lives in
// ingest_progress_test.cpp): just the transport override the hermetic job
// test injects through the provider's transport stub.
class FetchWorkerTestAccess {
 public:
  static void setPullTopicsOverride(
      FetchWorker& worker, std::function<void(const FetchWorker::OnBatch&, const FetchWorker::OnDone&)> pull_topics) {
    worker.pull_topics_override_ = std::move(pull_topics);
  }
};

}  // namespace mosaico::testing

namespace {

using mosaico::DescriptorImportProvider;
using mosaico::SourceDescriptor;

// The process trust allowlist, cleared around every test.
struct TrustEnvGuard {
  TrustEnvGuard() {
    ::unsetenv("MOSAICO_TRUSTED_ORIGINS");
  }
  ~TrustEnvGuard() {
    ::unsetenv("MOSAICO_TRUSTED_ORIGINS");
  }

  void setTrustAllowlist(const char* origins) {
    ::setenv("MOSAICO_TRUSTED_ORIGINS", origins, 1);
  }
};

SourceDescriptor descriptor() {
  SourceDescriptor d;
  d.version = 1;
  d.kind = "mosaico.pull";
  d.origin = "demo.mosaico.dev:6726";
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

TEST(DescriptorImportQuery, MalformedDescriptorIsAContractFailure) {
  TrustEnvGuard env;
  DescriptorImportProvider provider;
  auto result = freshResult();
  PJ_error_t error{};
  const std::string bad = "{not json";
  EXPECT_FALSE(provider.queryDescriptor(view(bad), &result, &error));
}

TEST(DescriptorImportQuery, PublishesPresentationThroughHostSettings) {
  TrustEnvGuard env;
  PJ::sdk::InMemorySettingsBackend settings_backend;
  PJ::sdk::SettingsStoreHost settings_host(settings_backend);
  DescriptorImportProvider provider;
  provider.bind(PJ::sdk::SettingsView{settings_host.view()}, {});
  const SourceDescriptor d = descriptor();
  const std::string json = mosaico::toSourceDescriptorJson(d);

  auto result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));

  const std::string group = PJ::sdk::source::sourcePresentationSettingsGroup(mosaico::descriptorIdentity(d));
  EXPECT_EQ(settings_backend.getString(group + "/display_name"), std::optional<std::string>("contract-test"));
  EXPECT_EQ(settings_backend.getString(group + "/origin"), std::optional<std::string>("demo.mosaico.dev:6726"));
}

TEST(DescriptorImportQuery, UntrustedThenEnvAllowlisted) {
  TrustEnvGuard env;
  DescriptorImportProvider provider;
  const SourceDescriptor d = descriptor();
  const std::string json = mosaico::toSourceDescriptorJson(d);

  // 1. Valid + untrusted: the host owns the cache in the M3 flow, so the
  // provider never reports a materialization or a size estimate.
  auto result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
  EXPECT_EQ(result.trust, PJ_DESCRIPTOR_TRUST_NEEDS_CONFIRMATION);
  EXPECT_EQ(result.is_materialized, 0u);
  EXPECT_EQ(str(result.source_identity), mosaico::descriptorIdentity(d));
  EXPECT_EQ(
      str(result.message), "server not trusted on this machine; confirm the download or set MOSAICO_TRUSTED_ORIGINS");
  EXPECT_EQ(result.estimated_bytes, 0u);

  // 2. An exact strict origin in the process allowlist flips the verdict.
  // The schemeless descriptor origin resolves to grpc+tls:// by default.
  env.setTrustAllowlist("https://ignored:443, grpc+tls://other.example:6726, GRPC+TLS://DEMO.MOSAICO.DEV:6726");
  result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
  EXPECT_EQ(result.trust, PJ_DESCRIPTOR_TRUST_TRUSTED);
  EXPECT_EQ(result.is_materialized, 0u);
  EXPECT_TRUE(str(result.message).empty());
}

TEST(DescriptorImportQuery, TrustAllowlistFailsClosedForDifferentOrMalformedOrigins) {
  TrustEnvGuard env;
  DescriptorImportProvider provider;
  const std::string json = mosaico::toSourceDescriptorJson(descriptor());

  for (const char* allowlist : {
           "",
           "not-an-origin,https://demo.mosaico.dev:6726",
           "grpc://demo.mosaico.dev:6726",  // scheme mismatch: headless target is grpc+tls
           "grpc+tls://demo.mosaico.dev:6727",
           "grpc+tls://other.mosaico.dev:6726",
           "demo.mosaico.dev:6726",  // schemeless entries never parse
       }) {
    env.setTrustAllowlist(allowlist);
    auto result = freshResult();
    ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
    EXPECT_EQ(result.trust, PJ_DESCRIPTOR_TRUST_NEEDS_CONFIRMATION) << allowlist;
  }
}

TEST(DescriptorImportQuery, StoredPlaintextOptInMovesTheTrustTarget) {
  // A per-machine allow_insecure opt-in makes the headless target grpc://,
  // so the trust check follows: the grpc+tls entry no longer matches and
  // the grpc entry does.
  TrustEnvGuard env;
  PJ::sdk::InMemorySettingsBackend settings_backend;
  PJ::sdk::SettingsStoreHost settings_host(settings_backend);
  DescriptorImportProvider provider;
  provider.bind(PJ::sdk::SettingsView{settings_host.view()}, {});
  const SourceDescriptor d = descriptor();
  const std::string json = mosaico::toSourceDescriptorJson(d);
  mosaico::ServerCredentials stored;
  stored.allow_insecure = true;
  mosaico::saveCredentialsForUri(PJ::sdk::SettingsView{settings_host.view()}, d.origin, stored);

  env.setTrustAllowlist("grpc+tls://demo.mosaico.dev:6726");
  auto result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
  EXPECT_EQ(result.trust, PJ_DESCRIPTOR_TRUST_NEEDS_CONFIRMATION);

  env.setTrustAllowlist("grpc://demo.mosaico.dev:6726");
  result = freshResult();
  ASSERT_TRUE(provider.queryDescriptor(view(json), &result, nullptr));
  EXPECT_EQ(result.trust, PJ_DESCRIPTOR_TRUST_TRUSTED);
}

// A no-op but VALID host binding: preflight rejections must fire before any
// host access, so these views are never actually used.
mosaico::DescriptorImportProvider::HostBindings stubBindings() {
  return {[] { return PJ::sdk::ToolboxHostView{}; }, [] { return PJ::ToolboxRuntimeHostView{}; }};
}

std::string errorMessage(const PJ_error_t& error) {
  return error.message;  // inline, null-terminated by fillError
}

TEST(DescriptorImportStart, FailClosedRejections) {
  TrustEnvGuard env;
  // BOUND provider: each rejection below must trip on its own named check,
  // not on the unbound-host fallthrough (an always-false stub would otherwise
  // pass this matrix).
  DescriptorImportProvider provider;
  provider.bind(PJ::sdk::SettingsView{}, stubBindings());
  const std::string json = mosaico::toSourceDescriptorJson(descriptor());

  PJ_descriptor_import_callbacks_v1_t callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.on_terminal = [](void*, PJ_descriptor_import_outcome_t, PJ_string_view_t) noexcept {};

  PJ_descriptor_import_start_request_v1_t request{};
  request.struct_size = sizeof(request);
  request.descriptor_json = view(json);

  // out_job must stay byte-for-byte UNTOUCHED on every rejection.
  PJ_joinable_job_t job{};
  std::memset(&job, 0xA5, sizeof(job));
  PJ_joinable_job_t job_snapshot{};
  std::memcpy(&job_snapshot, &job, sizeof(job));
  const auto job_untouched = [&] { return std::memcmp(&job, &job_snapshot, sizeof(job)) == 0; };

  PJ_error_t error{};

  // Null request / null out_job.
  EXPECT_FALSE(provider.startImport(nullptr, &callbacks, nullptr, &job, &error));
  EXPECT_NE(errorMessage(error).find("null request/out_job"), std::string::npos) << errorMessage(error);
  EXPECT_FALSE(provider.startImport(&request, &callbacks, nullptr, nullptr, &error));
  EXPECT_NE(errorMessage(error).find("null request/out_job"), std::string::npos) << errorMessage(error);
  EXPECT_TRUE(job_untouched());

  // Unknown flag bits fail closed FIRST.
  auto flagged = request;
  flagged.flags = UINT64_C(1) << 42;
  error = {};
  EXPECT_FALSE(provider.startImport(&flagged, &callbacks, nullptr, &job, &error));
  EXPECT_NE(errorMessage(error).find("flag"), std::string::npos) << errorMessage(error);
  EXPECT_TRUE(job_untouched());

  // on_terminal is required.
  auto no_terminal = callbacks;
  no_terminal.on_terminal = nullptr;
  error = {};
  EXPECT_FALSE(provider.startImport(&request, &no_terminal, nullptr, &job, &error));
  EXPECT_NE(errorMessage(error).find("on_terminal"), std::string::npos) << errorMessage(error);
  error = {};
  EXPECT_FALSE(provider.startImport(&request, nullptr, nullptr, &job, &error));
  EXPECT_NE(errorMessage(error).find("on_terminal"), std::string::npos) << errorMessage(error);
  auto short_callbacks = callbacks;
  short_callbacks.struct_size = offsetof(PJ_descriptor_import_callbacks_v1_t, on_terminal);
  error = {};
  EXPECT_FALSE(provider.startImport(&request, &short_callbacks, nullptr, &job, &error));
  EXPECT_NE(errorMessage(error).find("on_terminal"), std::string::npos) << errorMessage(error);
  EXPECT_TRUE(job_untouched());

  // Malformed descriptor.
  auto bad_request = request;
  const std::string bad = "{}";
  bad_request.descriptor_json = view(bad);
  error = {};
  EXPECT_FALSE(provider.startImport(&bad_request, &callbacks, nullptr, &job, &error));
  EXPECT_NE(errorMessage(error).find("missing field"), std::string::npos) << errorMessage(error);
  EXPECT_TRUE(job_untouched());

  // A valid request against an UNBOUND provider is the last gate.
  DescriptorImportProvider unbound;
  error = {};
  EXPECT_FALSE(unbound.startImport(&request, &callbacks, nullptr, &job, &error));
  EXPECT_NE(errorMessage(error).find("not bound"), std::string::npos) << errorMessage(error);
  EXPECT_TRUE(job_untouched());
}

// The all-empty pull, end to end through the job body (hermetic transport
// stub): every topic finishes ok-but-empty, the provisional dataset is never
// kept, and the terminal must NOT be SUCCEEDED_EAGER_ONLY — a layout restore
// would otherwise receive success with a dead dataset handle.
TEST(DescriptorImportJob, AllEmptyPullReportsNoUsableDataset) {
  TrustEnvGuard env;
  DescriptorImportProvider provider;
  provider.bind(PJ::sdk::SettingsView{}, stubBindings());
  provider.setTransportStubForTest([](mosaico::FetchWorker& fetch) {
    mosaico::testing::FetchWorkerTestAccess::setPullTopicsOverride(fetch, [](const auto&, const auto& on_done) {
      on_done("/imu", mosaico::PullResult{});  // transport ok, window empty
    });
  });
  const std::string json = mosaico::toSourceDescriptorJson(descriptor());

  struct Terminal {
    std::atomic<bool> dataset{false};
    std::atomic<PJ_descriptor_import_outcome_t> outcome{PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY};
    std::string message;  // written before on_terminal returns, read after join
  } terminal;

  PJ_descriptor_import_callbacks_v1_t callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.on_dataset = [](void* ctx, PJ_data_source_handle_t) noexcept {
    static_cast<Terminal*>(ctx)->dataset.store(true);
  };
  callbacks.on_terminal = [](void* ctx, PJ_descriptor_import_outcome_t outcome, PJ_string_view_t message) noexcept {
    auto* out = static_cast<Terminal*>(ctx);
    out->outcome.store(outcome);
    out->message = str(message);
  };

  PJ_descriptor_import_start_request_v1_t request{};
  request.struct_size = sizeof(request);
  request.descriptor_json = view(json);

  PJ_joinable_job_t job{};
  ASSERT_TRUE(provider.startImport(&request, &callbacks, &terminal, &job, nullptr));
  ASSERT_NE(job.vtable, nullptr);
  job.vtable->join(job.ctx);

  EXPECT_FALSE(terminal.dataset.load());
  EXPECT_EQ(terminal.outcome.load(), PJ_DESCRIPTOR_IMPORT_FAILED);
  EXPECT_NE(terminal.message.find("no usable dataset"), std::string::npos) << terminal.message;
  job.vtable->destroy(job.ctx);
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
      PJ::sdk::source::ProviderJob::start(
          [&](PJ::sdk::source::JobControl& control) {
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
            return PJ::sdk::source::ImportOutcome{PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY, "transfer completed"};
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
