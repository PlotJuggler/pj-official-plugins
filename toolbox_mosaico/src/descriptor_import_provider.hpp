// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// DescriptorImportProvider — the pj.descriptor_import.v1 implementation for
// Mosaico (the mcap_cloud provider shape, adapted to this plugin's
// FetchWorker + CacheTeeSession stage-1 machinery). Owned by MosaicoToolbox,
// which exposes it through its pluginExtension() thunks (the ABI's plugin_ctx
// is the TOOLBOX instance — never this object or the extension table).
//
// Surfaces, both operating on the raw C structs so the growth contract
// (struct_size-covered reads/writes) lives in exactly one place:
//   - queryDescriptor: [main-thread, strictly bounded] — descriptor parse,
//     in-memory trust lookup (preloaded once from the TrustedOrigins ledger),
//     DISK-validated cache lookup with lease-then-validate pinning, file-size
//     estimate. NO network, NO credential resolution, NO blocking lock
//     acquisition. Result strings are owned by this instance and stay valid
//     until the NEXT query on it (the ABI lifetime rule).
// Trust gating is the HOST's job by the ABI's design: queryDescriptor
// returns the verdict and the host confirms/refuses before calling
// startImport (its own confirmation flow may proceed past a
// needs-confirmation verdict, so the provider must NOT hard-gate here).
//
//   - startImport: [main-thread] — flags fail closed FIRST; required
//     callbacks validated; descriptor parsed; credentials resolved into an
//     immutable snapshot ON THIS THREAD (SettingsView is main-thread-only);
//     out_job populated BEFORE the worker starts; an explicit post-return
//     START GATE guarantees no callback runs before start_import returns.
//
// The job (one worker thread + one private FetchWorker per job): connect ->
// per-topic metadata (ontology routing) -> the descriptor-armed pull (cache
// tee + promotion) -> cancel-aware promotion-settlement wait (DETACH on
// cancel) -> the exactly-once terminal. cancel() is idempotent and
// non-blocking; join() returns after on_terminal returned; destroy() is
// cancel+join+free. NEVER call join/destroy from a job callback.
//
// NOTHING here touches the dialog/Qt: the provider works on an instance whose
// getDialog() was never called.
#pragma once

#include <pj_base/descriptor_import_protocol.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <pj_base/sdk/descriptor_import.hpp>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/file_lock.h"
#include "session_file_cache.hpp"
#include "trusted_origins.hpp"

namespace mosaico {

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

  /// Write-through trust recording (the dialog's connect-success hook):
  /// records into the durable TrustedOrigins ledger AND — only after that
  /// durable write succeeded — the in-memory set the query consults. False =
  /// nothing recorded anywhere (transient trust is never held silently).
  [[nodiscard]] bool recordSuccessfulConnect(const std::string& uri);

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
  struct JobState;

  static void jobCancel(void* ctx) noexcept;
  static void jobJoin(void* ctx) noexcept;
  static void jobDestroy(void* ctx) noexcept;
  static constexpr PJ_joinable_job_vtable_t kJobVtable{
      sizeof(PJ_joinable_job_vtable_t), 0, &DescriptorImportProvider::jobCancel, &DescriptorImportProvider::jobJoin,
      &DescriptorImportProvider::jobDestroy};

  /// The cache the provider answers from: the user-configured
  /// mosaico/cache_directory when set (the panel's Directory field), else
  /// standardCacheRoot — main thread only (SettingsView).
  [[nodiscard]] SessionFileCache makeFileCache();
  /// Preload the in-memory trust set from the ledger (once, lazily).
  void ensureTrustLoaded();

  PJ::sdk::SettingsView settings_{};
  HostBindings bindings_{};
  TrustedOrigins ledger_ = TrustedOrigins::standard();
  std::mutex trust_mu_;
  bool trust_loaded_ = false;
  std::unordered_set<std::string> trusted_keys_;  // trustedOriginKey shape

  // Materialized-hit read leases pinned by the query (a path the HOST is
  // about to load must survive another process's eviction while the loader
  // lazily re-opens it). v1 approximation: leases live as long as this
  // provider (the toolbox instance). Main-thread only, like the query itself.
  std::unordered_map<std::string, FileLock> query_leases_;

  // Promotion leases adopted from finished import jobs (the per-job
  // FetchWorker dies with its JobState right after on_terminal, but the
  // promoted dataset keeps lazily re-opening the artifact — the lease must
  // outlive the job). Appended from job threads; guarded.
  std::mutex job_leases_mu_;
  std::unordered_map<std::string, FileLock> job_leases_;

  // query-result string storage: owned by the provider, valid until the NEXT
  // query on this instance (ABI lifetime rule). Main-thread only.
  std::string query_identity_;
  std::string query_path_;
  std::string query_message_;

  // test seams
  std::function<void()> start_gate_probe_;
  std::function<void()> pre_terminal_hook_;
  std::chrono::milliseconds poll_{50};
};

}  // namespace mosaico
