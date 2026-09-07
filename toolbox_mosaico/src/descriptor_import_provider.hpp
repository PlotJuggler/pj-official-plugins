// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// DescriptorImportProvider — the pj.descriptor_import.v1 implementation for
// Mosaico, the THIN M3 provider: the HOST owns caching and materialization,
// so this provider only re-runs the recorded Download (connect + pull) when
// asked. Owned by MosaicoToolbox, which exposes it through its
// pluginExtension() thunks (the ABI's plugin_ctx is the TOOLBOX instance —
// never this object or the extension table).
//
// Surfaces, both operating on the raw C structs through the SDK's growth-
// contract helpers:
//   - queryDescriptor: [main-thread, strictly bounded] — descriptor parse,
//     identity, presentation write through the host settings view, and the
//     environment trust allowlist lookup. NO network, NO credential
//     resolution. is_materialized is always 0 and local_path is empty: the
//     host classifies hit/miss against ITS OWN cache by identity. Result
//     strings are owned by this instance and stay valid until the NEXT query
//     on it (the ABI lifetime rule). Trust gating is the HOST's job by the
//     ABI's design: the query returns the verdict and the host
//     confirms/refuses before calling startImport (its own confirmation flow
//     may proceed past a needs-confirmation verdict, so the provider must
//     NOT hard-gate here). A parse failure is a CONTRACT failure (query
//     returns false), never a trust verdict.
//
//   - startImport: [main-thread] — flags fail closed FIRST; required
//     callbacks validated; descriptor parsed; credentials resolved into an
//     immutable snapshot ON THIS THREAD (SettingsView is main-thread-only);
//     ProviderJob owns the worker, start gate, cancellation, watchdog, and
//     the exactly-once terminal.
//
// The job (one SDK-owned worker thread + one private FetchWorker per job):
// connect -> listTopics (metadata cache for ontology routing) -> the pull
// (which attaches the canonical source record + whole-request completion
// through the host ingest bracket) -> the exactly-once terminal. cancel() is
// idempotent and non-blocking; join() returns after on_terminal returned;
// destroy() is cancel+join+free. NEVER call join/destroy from a job callback.
//
// NOTHING here touches the dialog/Qt: the provider works on an instance whose
// getDialog() was never called.
#pragma once

#include <pj_base/descriptor_import_protocol.h>

#include <atomic>
#include <functional>
#include <pj_base/sdk/descriptor_import.hpp>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <string>

namespace mosaico {

class FetchWorker;

namespace testing {
class DescriptorImportProviderTestAccess;
}

class DescriptorImportProvider {
 public:
  /// The host access a job's pull ingests through (the toolbox's
  /// toolboxHost() / runtimeHost(), wrapped as providers exactly like the
  /// dialog's worker).
  struct HostBindings {
    std::function<PJ::sdk::ToolboxHostView()> host_provider;
    std::function<PJ::ToolboxRuntimeHostView()> runtime_host_provider;
  };

  DescriptorImportProvider();
  ~DescriptorImportProvider();
  DescriptorImportProvider(const DescriptorImportProvider&) = delete;
  DescriptorImportProvider& operator=(const DescriptorImportProvider&) = delete;

  /// Main-thread wiring at bind() (re-bind swaps the views; no network).
  void bind(PJ::sdk::SettingsView settings, HostBindings bindings);

  /// The raw extension surface (see the file header). The toolbox's
  /// PJ_descriptor_import_provider_v1_t thunks forward here.
  bool queryDescriptor(
      PJ_string_view_t descriptor_json, PJ_descriptor_query_result_v1_t* out_result, PJ_error_t* out_error);
  bool startImport(
      const PJ_descriptor_import_start_request_v1_t* request, const PJ_descriptor_import_callbacks_v1_t* callbacks,
      void* callback_ctx, PJ_joinable_job_t* out_job, PJ_error_t* out_error);

  /// Test seam (main-thread, set before start_import): replaces the job's
  /// connect + listTopics transport setup — the stub typically installs a
  /// pull override on the worker — so the job path runs hermetically.
  void setTransportStubForTest(std::function<void(FetchWorker&)> stub) {
    transport_stub_for_test_ = std::move(stub);
  }

 private:
  friend class testing::DescriptorImportProviderTestAccess;

  struct JobState;

  /// Claim a still-active transport deadline. A watchdog firing after the
  /// pull's disarm observes false and must not change the job outcome.
  [[nodiscard]] static bool claimTransferWatchdogExpiry(std::atomic<bool>& transfer_in_progress) noexcept;

  PJ::sdk::SettingsView settings_{};
  HostBindings bindings_{};
  std::function<void(FetchWorker&)> transport_stub_for_test_;

  // query-result string storage: owned by the provider, valid until the NEXT
  // query on this instance (ABI lifetime rule). Main-thread only.
  PJ::DescriptorQueryResult query_result_;
};

}  // namespace mosaico
