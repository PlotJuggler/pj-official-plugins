// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// See descriptor_import_provider.hpp for the contract. The job runner's
// terminal mapping — including the locked concurrent-materialization race
// decision inherited from the mcap_cloud provider — is documented inline at
// runToTerminal().
#include "descriptor_import_provider.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <pj_base/sdk/descriptor_import/origin.hpp>
#include <pj_base/sdk/descriptor_import/provider_job.hpp>
#include <pj_base/sdk/platform.hpp>
#include <pj_base/sdk/testing/provider_job_probe.hpp>
#include <string_view>
#include <utility>
#include <vector>

#include "cache_policy.hpp"
#include "credential_resolve.hpp"
#include "descriptor_import/arrow_cache_artifact.hpp"
#include "descriptor_import/source_descriptor.hpp"
#include "fetch_worker.hpp"
#include "promotion_artifact_lease_registry.hpp"
#include "source_presentation.hpp"

namespace mosaico {

namespace {

std::string_view toView(PJ_string_view_t sv) {
  return std::string_view(sv.data == nullptr ? "" : sv.data, sv.data == nullptr ? 0 : sv.size);
}

// Per-machine provider hard limits: a trusted server must not be able to
// stream indefinitely just because the CALLER passed no ceiling. Generous
// defaults, env-overridable, 0 = explicitly off.
constexpr std::uint64_t kDefaultImportMaxBytes = 32ull * 1024 * 1024 * 1024;  // 32 GiB
constexpr std::uint64_t kDefaultImportMaxSeconds = 3600;                      // 1 h

std::uint64_t envLimit(const char* name, std::uint64_t fallback) {
  const std::optional<std::string> raw = PJ::sdk::getEnv(name);
  if (!raw.has_value() || raw->empty()) {
    return fallback;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(*raw));
  } catch (...) {
    return fallback;  // unparsable -> the safe default, never off
  }
}

// min-nonzero merge: the effective ceiling is the tighter of the caller's and
// the provider's; 0 on either side means "that side imposes none".
std::uint64_t minNonzero(std::uint64_t a, std::uint64_t b) {
  if (a == 0) {
    return b;
  }
  if (b == 0) {
    return a;
  }
  return a < b ? a : b;
}

bool trustedByEnvironment(std::string_view server_uri) {
  const std::optional<std::string> allowlist = PJ::sdk::getEnv("MOSAICO_TRUSTED_ORIGINS");
  if (!allowlist.has_value()) {
    return false;
  }
  const PJ::sdk::descriptor_import::OriginPolicy policy{{"grpc", "grpc+tls"}, {}};
  return PJ::sdk::descriptor_import::originAllowed(
      server_uri, PJ::sdk::descriptor_import::parseOriginList(*allowlist, policy), policy);
}

}  // namespace

// ---------------------------------------------------------------------------
// JobState — Mosaico's policy/body state. ProviderJob owns the lifecycle.
// ---------------------------------------------------------------------------

struct DescriptorImportProvider::JobState {
  explicit JobState(PJ::sdk::descriptor_import::RequestArtifactCache cache) : file_cache(std::move(cache)) {}

  // ---- immutable inputs (written by startImport BEFORE the worker starts) --
  HostBindings bindings;
  SourceDescriptor descriptor;
  std::string identity;
  ServerCredentials credentials;  // resolved on the main thread
  std::string cache_root_override;
  PJ::sdk::descriptor_import::RequestArtifactCache file_cache;  // same root artifact capture publishes under
  std::uint64_t max_transfer_bytes = 0;
  std::chrono::milliseconds max_transfer_duration{0};
  std::chrono::milliseconds poll{50};
  std::function<void()> pre_terminal_hook;                 // test seam
  PJ::sdk::descriptor_import::CleanupPolicy cache_policy;  // resolved on the main thread

  FetchWorker fetch;  // per-job: owns the client + cancel wake machinery

  // ---- per-run result state ------------------------------------------------
  std::shared_ptr<PJ::sdk::descriptor_import::SettlementLatch> promotion_latch =
      std::make_shared<PJ::sdk::descriptor_import::SettlementLatch>();
  std::atomic<bool> dataset_created{false};
  std::atomic<bool> host_stop_requested{false};
  std::atomic<bool> imported_any{false};
  std::atomic<bool> any_failed{false};
  std::atomic<bool> byte_ceiling_exceeded{false};
  std::atomic<bool> duration_ceiling_exceeded{false};
  std::atomic<bool> transfer_in_progress{false};
  std::mutex results_mu;
  std::string first_error;                             // guarded by results_mu
  std::map<std::string, std::int64_t> bytes_by_topic;  // guarded by results_mu
  std::uint64_t total_bytes = 0;                       // guarded by results_mu

  [[nodiscard]] bool isCancelled(const PJ::sdk::descriptor_import::JobControl& control) const {
    return control.isCancelled() || host_stop_requested.load(std::memory_order_relaxed);
  }

  PJ::sdk::descriptor_import::ImportOutcome runToTerminal(PJ::sdk::descriptor_import::JobControl& control);
};

bool DescriptorImportProvider::claimTransferWatchdogExpiry(std::atomic<bool>& transfer_in_progress) noexcept {
  return transfer_in_progress.exchange(false, std::memory_order_relaxed);
}

PJ::sdk::descriptor_import::ImportOutcome DescriptorImportProvider::JobState::runToTerminal(
    PJ::sdk::descriptor_import::JobControl& control) {
  control.onCancel([this] { fetch.requestCancel(); });
  if (isCancelled(control)) {
    return {PJ_DESCRIPTOR_IMPORT_CANCELLED, "import cancelled"};
  }

  // The host classifies hit/miss BEFORE starting a job, so a fresh VALID
  // cache file here means a concurrent materialization won the race (the
  // interactive fetch, or another process). No eager ingest ran here (ZERO
  // on_dataset), so the only truthful terminal is a retry classification —
  // never promote, never report success (the mcap_cloud provider's locked v1
  // decision). Ongoing (unfinished) materializations are arbitrated later by
  // the capture's materialize lock: capture simply disarms and this import lands
  // eager-only.
  {
    if (file_cache.lookup(identity)) {
      return {
          PJ_DESCRIPTOR_IMPORT_FAILED, "session was materialized concurrently; reload to classify it as a cache hit"};
    }
  }

  // The worker's own maintenance passes (before capture, after publish)
  // apply this job's budget.
  fetch.setCacheCleanupPolicy(cache_policy);
  fetch.setHostProvider(bindings.host_provider);
  fetch.setRuntimeHostProvider(bindings.runtime_host_provider);
  fetch.setPromotionProvider(bindings.promotion_provider);

  // on_dataset: zero-or-one, fired when the pull creates its batch dataset,
  // strictly before any publication into it.
  fetch.datasetCreated = [this, &control](PJ::sdk::DataSourceHandle handle) {
    if (!dataset_created.exchange(true)) {
      control.notifyDataset(PJ_data_source_handle_t{handle.id});
    }
  };
  // Per-topic outcomes (SDK pool threads): the terminal message carries the
  // first failure verbatim.
  fetch.pullFinished = [this](PullResultEvent event) {
    if (event.ok) {
      imported_any.store(true, std::memory_order_relaxed);
      return;
    }
    any_failed.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(results_mu);
    if (first_error.empty() && !event.error.empty()) {
      first_error = event.topic.topic_name.empty() ? event.error : (event.topic.topic_name + ": " + event.error);
    }
  };
  fetch.errorOccurred = [this](std::string error) {
    std::lock_guard<std::mutex> lock(results_mu);
    if (first_error.empty()) {
      first_error = std::move(error);
    }
  };
  // Byte-ceiling enforcement over the SDK's true decoded-size progress: the
  // fetch-level cancel (NOT the job cancel flag) aborts the transfer, so the
  // terminal classifies as the ceiling, not as a caller cancel.
  fetch.pullProgress = [this](std::string topic_name, std::int64_t bytes) {
    if (max_transfer_bytes == 0) {
      return;
    }
    std::uint64_t total = 0;
    {
      // `bytes` is the topic's cumulative count: replace its ledger entry and
      // nudge the running total by the delta (no per-tick rescan).
      std::lock_guard<std::mutex> lock(results_mu);
      std::int64_t& entry = bytes_by_topic[topic_name];
      total_bytes += static_cast<std::uint64_t>(std::max<std::int64_t>(bytes, 0)) -
                     static_cast<std::uint64_t>(std::max<std::int64_t>(entry, 0));
      entry = bytes;
      total = total_bytes;
    }
    if (total > max_transfer_bytes && !byte_ceiling_exceeded.exchange(true)) {
      fetch.requestCancel();
    }
  };
  // A host-side Stop (the curve-tree row's stop button) is a caller cancel.
  fetch.hostStopRequested = [this] { host_stop_requested.store(true, std::memory_order_relaxed); };
  // Promotion settlement rides the SDK's shared, settle-exactly-once latch.
  {
    auto latch = promotion_latch;
    fetch.promotionSettled = [latch](bool ok, std::string detail) { latch->settle(ok, std::move(detail)); };
  }

  // ---- connect (blocking; the client's own 30 s timeout bounds it) ---------
  ConnectResult connect_result;
  fetch.connectFinished = [&connect_result](ConnectResult result) { connect_result = std::move(result); };
  fetch.connectAsync(descriptor.server_uri, credentials);
  if (isCancelled(control)) {
    return {PJ_DESCRIPTOR_IMPORT_CANCELLED, "import cancelled"};
  }
  if (!connect_result.ok) {
    return {PJ_DESCRIPTOR_IMPORT_FAILED, "could not connect to " + descriptor.server_uri + ": " + connect_result.error};
  }

  // ---- topic list metadata -------------------------------------------------
  // Preserve size/timestamp context when the server supports listTopics.
  // pullTopicsAsync itself fills every missing ontology tag, so interactive
  // and headless routing are identical by construction.
  fetch.listTopicsAsync(descriptor.sequence);

  // ---- the descriptor-armed pull (artifact capture + promotion) ------------
  // Duration-ceiling watchdog: fires the FETCH cancel (like the byte
  // ceiling) so the terminal classifies as the ceiling, not caller cancel.
  // ProviderJob owns the watchdog through the whole body, whereas Mosaico's
  // ceiling covers transport only. The active gate preserves that scope: a
  // later expiry during promotion settlement is inert.
  transfer_in_progress.store(true, std::memory_order_relaxed);
  control.armWatchdog(max_transfer_duration, [this] {
    if (!DescriptorImportProvider::claimTransferWatchdogExpiry(transfer_in_progress)) {
      return;
    }
    duration_ceiling_exceeded.store(true, std::memory_order_relaxed);
    fetch.requestCancel();
  });
  fetch.pullTopicsAsync(
      descriptor.sequence, descriptor.topics, descriptor.start_ns, descriptor.end_ns, descriptor, cache_root_override);
  transfer_in_progress.store(false, std::memory_order_relaxed);
  if (pre_terminal_hook) {
    pre_terminal_hook();
  }

  // ---- terminal mapping ----------------------------------------------------
  // Precedence (the mcap_cloud provider's pinned order): for a NON-COMPLETE
  // pull, an explicit caller cancel wins — even over a ceiling when both
  // latched; the ceilings then win over every other failure cause. A batch
  // that COMPLETED before a post-completion cancel reports its truthful
  // terminal (the host's cancel rollback is ledger-based either way).
  const bool batch_complete = imported_any.load() && !any_failed.load();
  if (isCancelled(control) && !batch_complete) {
    return {PJ_DESCRIPTOR_IMPORT_CANCELLED, "import cancelled"};
  }
  if (byte_ceiling_exceeded.load()) {
    return {PJ_DESCRIPTOR_IMPORT_FAILED, "import exceeded the transfer byte ceiling (MOSAICO_IMPORT_MAX_BYTES)"};
  }
  if (duration_ceiling_exceeded.load()) {
    return {PJ_DESCRIPTOR_IMPORT_FAILED, "import exceeded the transfer time ceiling (MOSAICO_IMPORT_MAX_SECONDS)"};
  }
  if (!imported_any.load()) {
    std::lock_guard<std::mutex> lock(results_mu);
    return {PJ_DESCRIPTOR_IMPORT_FAILED, first_error.empty() ? "no topic could be imported" : first_error};
  }

  // ---- promotion settlement (cancel-aware, DETACH on cancel) ---------------
  // The FetchWorker fires promotionSettled on every armed-capture path; an
  // ACCEPTED promotion settles whenever the host's result callback runs —
  // possibly after the pull returned. A cancel while it is outstanding
  // detaches: the shared latch outlives this job and join() stays
  // unblockable.
  const bool settled = promotion_latch->wait([this, &control] { return isCancelled(control); }, poll);
  if (!settled) {
    return {PJ_DESCRIPTOR_IMPORT_CANCELLED, "import cancelled (promotion still pending; detached)"};
  }
  const bool eager_usable = dataset_created.load() && imported_any.load();
  if (promotion_latch->ok()) {
    return {PJ_DESCRIPTOR_IMPORT_SUCCEEDED_PROMOTED, "promoted to a file-backed source"};
  }
  if (eager_usable) {
    return {
        PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY,
        "import completed without promotion (" + promotion_latch->detail() + ")"};
  }
  return {PJ_DESCRIPTOR_IMPORT_FAILED, "import produced no usable dataset (" + promotion_latch->detail() + ")"};
}

// ---------------------------------------------------------------------------
// DescriptorImportProvider
// ---------------------------------------------------------------------------

DescriptorImportProvider::DescriptorImportProvider() = default;

DescriptorImportProvider::~DescriptorImportProvider() = default;

void DescriptorImportProvider::bind(PJ::sdk::SettingsView settings, HostBindings bindings) {
  settings_ = settings;
  bindings_ = std::move(bindings);
}

PJ::sdk::descriptor_import::RequestArtifactCache DescriptorImportProvider::makeFileCache() {
  std::string configured;
  if (auto stored = settings_.value("mosaico/cache_directory")) {
    configured = stored->toString();
  }
  return makeArtifactCache(std::filesystem::path(configured));
}

bool DescriptorImportProvider::queryDescriptor(
    PJ_string_view_t descriptor_json, PJ_descriptor_query_result_v1_t* out_result, PJ_error_t* out_error) {
  try {
    if (out_result == nullptr) {
      PJ::sdk::fillError(out_error, 1, "mosaico", "null out_result");
      return false;
    }
    std::string parse_error;
    const std::optional<SourceDescriptor> descriptor = parseSourceDescriptor(toView(descriptor_json), &parse_error);
    if (!descriptor.has_value()) {
      // Malformed OR unsupported: a CONTRACT failure (query returns false),
      // never a trust verdict.
      PJ::sdk::fillError(out_error, 1, "mosaico", parse_error);
      return false;
    }

    auto cache = makeFileCache();
    // Result strings: owned by this instance, valid until the NEXT query on
    // it (the ABI lifetime rule; main-thread only, like the query itself).
    query_result_ = PJ::DescriptorQueryResult{};
    query_result_.source_identity = descriptorIdentity(*descriptor);
    // The host builds the dialog rows only after this bounded query returns.
    // Publish the descriptor's human presentation now so first loads on a new
    // machine do not fall back to the opaque durable identity.
    recordSourcePresentation(settings_, query_result_.source_identity, *descriptor);
    query_result_.local_path_utf8 = utf8Path(cache.pathFor(query_result_.source_identity));

    // Trust: strict origin equality against the process environment
    // allowlist. Malformed entries are ignored; v1 emits only trusted /
    // needs-confirmation (refused is reserved for future policy).
    const bool trusted = trustedByEnvironment(descriptor->server_uri);
    query_result_.trust = trusted ? PJ::DescriptorTrust::kTrusted : PJ::DescriptorTrust::kNeedsConfirmation;
    if (!trusted) {
      query_result_.message = "server not trusted on this machine; confirm the download or set MOSAICO_TRUSTED_ORIGINS";
    }

    // is_materialized: the DISK-validated cache ONLY, with lease-then-
    // validate pinning — a materialized hit is a path the HOST is about to
    // load, and after a process restart no finalize-time lease exists. Retain
    // the lease in the DSO-lifetime path registry so panel/provider teardown
    // cannot make the host's lazy re-open evictable. The SDK never returns an
    // unleased path: lock contention is a miss with a retry hint.
    const auto append_message = [this](std::string message) {
      if (!query_result_.message.empty()) {
        query_result_.message += "; ";
      }
      query_result_.message += std::move(message);
    };
    std::filesystem::path disk;
    std::string miss_reason;
    if (auto hit = cache.lookup(query_result_.source_identity, &miss_reason)) {
      disk = hit->path;
      promotionArtifactLeaseRegistry().retain(disk, std::move(hit->lease));
      query_result_.is_materialized = true;
    } else if (miss_reason.find("retry") != std::string::npos) {
      append_message("cache artifact is temporarily locked; retry the import");
    }
    if (query_result_.is_materialized) {
      std::error_code ec;
      const auto size = std::filesystem::file_size(disk, ec);
      if (!ec) {
        query_result_.estimated_bytes = static_cast<std::uint64_t>(size);
      }
    }

    PJ::writeDescriptorQueryResult(out_result, query_result_);
    return true;
  } catch (...) {
    PJ::sdk::fillError(out_error, 1, "mosaico", "internal error in query_descriptor");
    return false;
  }
}

bool DescriptorImportProvider::startImport(
    const PJ_descriptor_import_start_request_v1_t* request, const PJ_descriptor_import_callbacks_v1_t* callbacks,
    void* callback_ctx, PJ_joinable_job_t* out_job, PJ_error_t* out_error) {
  try {
    // Preserve Mosaico's synchronous ABI-preflight precedence. ProviderJob
    // revalidates this surface at the tail call, after Mosaico has parsed and
    // resolved its policy, but unusable outputs/callbacks must not make that
    // policy work observable.
    if (request == nullptr || out_job == nullptr) {
      PJ::sdk::fillError(out_error, 1, "mosaico", "null request/out_job");
      return false;
    }
    auto parsed_request = PJ::readDescriptorImportStartRequest(request);
    if (!parsed_request) {
      PJ::sdk::fillError(out_error, 1, "mosaico", parsed_request.error());
      return false;
    }
    const bool terminal_covered =
        callbacks != nullptr && PJ::sdk::fieldCovered(
                                    callbacks->struct_size, offsetof(PJ_descriptor_import_callbacks_v1_t, on_terminal),
                                    sizeof(callbacks->on_terminal));
    if (!terminal_covered || callbacks->on_terminal == nullptr) {
      PJ::sdk::fillError(out_error, 1, "mosaico", "on_terminal callback is required");
      return false;
    }

    std::string parse_error;
    const std::optional<SourceDescriptor> descriptor =
        parseSourceDescriptor(parsed_request->descriptor_json, &parse_error);
    if (!descriptor.has_value()) {
      PJ::sdk::fillError(out_error, 1, "mosaico", parse_error);
      return false;
    }
    if (!bindings_.host_provider || !bindings_.runtime_host_provider) {
      PJ::sdk::fillError(out_error, 1, "mosaico", "descriptor import provider is not bound to a host");
      return false;
    }

    // Credentials resolved ON THIS (main) thread — SettingsView is
    // main-thread-only; the job thread never touches the view. The env-key
    // origin guard lives in resolveHeadlessCredentials.
    auto state = std::make_shared<JobState>(makeFileCache());
    state->bindings = bindings_;
    state->descriptor = *descriptor;
    state->identity = descriptorIdentity(*descriptor);
    state->credentials = resolveHeadlessCredentials(settings_, descriptor->server_uri);
    if (auto stored = settings_.value("mosaico/cache_directory")) {
      state->cache_root_override = stored->toString();
    }
    // The provider's per-machine hard limits apply even when the caller
    // imposes none — effective = min-nonzero(caller, provider). Read on the
    // main thread, like every other start-time resolution.
    state->max_transfer_bytes =
        minNonzero(parsed_request->max_transfer_bytes, envLimit("MOSAICO_IMPORT_MAX_BYTES", kDefaultImportMaxBytes));
    state->max_transfer_duration =
        std::chrono::milliseconds(envLimit("MOSAICO_IMPORT_MAX_SECONDS", kDefaultImportMaxSeconds) * 1000);
    state->poll = poll_;
    state->cache_policy = cacheCleanupPolicyFromSettings(settings_);
    state->pre_terminal_hook = pre_terminal_hook_;
    auto body = [state](PJ::sdk::descriptor_import::JobControl& control) { return state->runToTerminal(control); };

    // Keep the deterministic gate probe as a test seam; production uses the
    // ordinary SDK entry point as start_import's tail call.
    if (start_gate_probe_) {
      return PJ::sdk::descriptor_import::testing::startWithGateProbe(
          std::move(body), callbacks, callback_ctx, out_job, out_error, start_gate_probe_);
    }
    return PJ::sdk::descriptor_import::ProviderJob::start(std::move(body), callbacks, callback_ctx, out_job, out_error);
  } catch (...) {
    PJ::sdk::fillError(out_error, 1, "mosaico", "internal error in start_import");
    return false;
  }
}

}  // namespace mosaico
