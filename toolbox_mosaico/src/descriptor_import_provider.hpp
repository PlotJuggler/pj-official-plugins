// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// DescriptorImportProvider — the pj.descriptor_import.v1 implementation for
// Mosaico (the mcap_cloud provider shape, adapted to this plugin's
// FetchWorker + ArtifactCapture stage-1 machinery). Owned by MosaicoToolbox,
// which exposes it through its pluginExtension() thunks (the ABI's plugin_ctx
// is the TOOLBOX instance — never this object or the extension table).
//
// Surfaces, both operating on the raw C structs through the SDK's growth-
// contract helpers:
//   - queryDescriptor: [main-thread, strictly bounded] — descriptor parse,
//     presentation write through the host settings view, environment trust
//     allowlist lookup, DISK-validated cache lookup with lease-then-validate
//     pinning, file-size estimate. NO network, NO credential resolution, NO
//     blocking lock acquisition. Result strings are owned by this instance and
//     stay valid until the NEXT query on it (the ABI lifetime rule).
// Trust gating is the HOST's job by the ABI's design: queryDescriptor
// returns the verdict and the host confirms/refuses before calling
// startImport (its own confirmation flow may proceed past a
// needs-confirmation verdict, so the provider must NOT hard-gate here).
//
//   - startImport: [main-thread] — flags fail closed FIRST; required
//     callbacks validated; descriptor parsed; credentials resolved into an
//     immutable snapshot ON THIS THREAD (SettingsView is main-thread-only);
//     ProviderJob owns the worker, start gate, cancellation, watchdog,
//     exactly-once terminal, and joinable-job vtable.
//
// The job (one SDK-owned worker thread + one private FetchWorker per job): connect ->
// per-topic metadata (ontology routing) -> the descriptor-armed pull (cache
// artifact capture + promotion) -> cancel-aware promotion-settlement wait
// (DETACH on cancel) -> the exactly-once terminal. cancel() is idempotent and
// non-blocking; join() returns after on_terminal returned; destroy() is
// cancel+join+free. NEVER call join/destroy from a job callback.
//
// NOTHING here touches the dialog/Qt: the provider works on an instance whose
// getDialog() was never called.
#pragma once

#include <pj_base/descriptor_import_protocol.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <pj_base/sdk/descriptor_import.hpp>
#include <pj_base/sdk/descriptor_import/request_cache.hpp>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <string>

namespace mosaico {

namespace testing {
class DescriptorImportProviderTestAccess;
}

class DescriptorImportProvider {
 public:
  /// The host access a job's pull ingests through (the toolbox's
  /// toolboxHost() / runtimeHost() / promotion view, wrapped as providers
  /// exactly like the dialog's worker).
  struct HostBindings {
    std::function<PJ::sdk::ToolboxHostView()> host_provider;
    std::function<PJ::ToolboxRuntimeHostView()> runtime_host_provider;
    std::function<PJ::SourcePromotionHostView()> promotion_provider;
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

  // ---- test seams (main-thread, set before start_import) -------------------
  /// Runs inside start_import AFTER out_job is populated and the (gated)
  /// worker exists, BEFORE the start gate is released.
  void setStartGateProbeForTest(std::function<void()> probe) {
    start_gate_probe_ = std::move(probe);
  }
  /// Runs on the job thread after the pull returned, BEFORE the terminal
  /// mapping.
  void setPreTerminalHookForTest(std::function<void()> hook) {
    pre_terminal_hook_ = std::move(hook);
  }
  /// Shrink the promotion-settlement / cancel poll interval.
  void setPollForTest(std::chrono::milliseconds poll) {
    poll_ = poll;
  }

 private:
  friend class testing::DescriptorImportProviderTestAccess;

  struct JobState;

  /// Claim a still-active transport deadline. A watchdog firing after the
  /// pull's disarm observes false and must not change the job outcome.
  [[nodiscard]] static bool claimTransferWatchdogExpiry(std::atomic<bool>& transfer_in_progress) noexcept;

  /// The cache the provider answers from: the user-configured
  /// mosaico/cache_directory when set (the panel's Directory field), else
  /// standardCacheRoot — main thread only (SettingsView).
  [[nodiscard]] PJ::sdk::descriptor_import::RequestArtifactCache makeFileCache();
  PJ::sdk::SettingsView settings_{};
  HostBindings bindings_{};

  // query-result string storage: owned by the provider, valid until the NEXT
  // query on this instance (ABI lifetime rule). Main-thread only.
  PJ::DescriptorQueryResult query_result_;

  // test seams
  std::function<void()> start_gate_probe_;
  std::function<void()> pre_terminal_hook_;
  std::chrono::milliseconds poll_{50};
};

}  // namespace mosaico
