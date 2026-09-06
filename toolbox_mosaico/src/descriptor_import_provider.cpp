// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// See descriptor_import_provider.hpp for the contract.
#include "descriptor_import_provider.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <pj_base/sdk/platform.hpp>
#include <pj_base/sdk/source/limits.hpp>
#include <pj_base/sdk/source/origin.hpp>
#include <pj_base/sdk/source/presentation.hpp>
#include <pj_base/sdk/source/provider_job.hpp>
#include <string_view>
#include <utility>
#include <vector>

#include "credential_resolve.hpp"
#include "fetch_worker.hpp"
#include "source_descriptor.hpp"

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

// Strict origin equality against the process environment allowlist.
// Malformed entries are ignored (an allowlist only ever widens by being
// correct); no entry, or no variable, means NOT trusted.
bool trustedByEnvironment(std::string_view server_uri) {
  const std::optional<std::string> allowlist = PJ::sdk::getEnv("MOSAICO_TRUSTED_ORIGINS");
  if (!allowlist.has_value()) {
    return false;
  }
  const PJ::sdk::source::OriginPolicy policy{{"grpc", "grpc+tls"}, {}};
  return PJ::sdk::source::originAllowed(server_uri, PJ::sdk::source::parseOriginList(*allowlist, policy), policy);
}

}  // namespace

// ---------------------------------------------------------------------------
// JobState — Mosaico's policy/body state. ProviderJob owns the lifecycle.
// ---------------------------------------------------------------------------

struct DescriptorImportProvider::JobState {
  // ---- immutable inputs (written by startImport BEFORE the worker starts) --
  HostBindings bindings;
  SourceDescriptor descriptor;
  std::string target_uri;         // scheme resolved on the main thread
  ServerCredentials credentials;  // resolved on the main thread
  // The one plaintext retry after a failed TLS connect; empty = not allowed.
  std::string plaintext_retry_uri;
  ServerCredentials plaintext_credentials;
  std::uint64_t max_transfer_bytes = 0;
  std::chrono::milliseconds max_transfer_duration{0};
  std::function<void(FetchWorker&)> transport_stub;  // test seam: replaces connect + listTopics

  FetchWorker fetch;  // per-job: owns the client + cancel wake machinery

  // ---- per-run result state ------------------------------------------------
  std::atomic<bool> dataset_created{false};
  std::atomic<bool> dataset_discarded{false};
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

  [[nodiscard]] bool isCancelled(const PJ::sdk::source::JobControl& control) const {
    return control.isCancelled() || host_stop_requested.load(std::memory_order_relaxed);
  }

  PJ::sdk::source::ImportOutcome runToTerminal(PJ::sdk::source::JobControl& control);
};

bool DescriptorImportProvider::claimTransferWatchdogExpiry(std::atomic<bool>& transfer_in_progress) noexcept {
  return transfer_in_progress.exchange(false, std::memory_order_relaxed);
}

PJ::sdk::source::ImportOutcome DescriptorImportProvider::JobState::runToTerminal(PJ::sdk::source::JobControl& control) {
  control.onCancel([this] { fetch.requestCancel(); });
  if (isCancelled(control)) {
    return {PJ_DESCRIPTOR_IMPORT_CANCELLED, "import cancelled"};
  }

  fetch.setHostProvider(bindings.host_provider);
  fetch.setRuntimeHostProvider(bindings.runtime_host_provider);

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
  // A rolled-back provisional dataset (zero-success or all-empty batch) makes
  // any announced handle dead: the terminal below must not claim it.
  fetch.datasetDiscarded = [this] { dataset_discarded.store(true, std::memory_order_relaxed); };

  if (transport_stub) {
    // Hermetic test path: the stub installs its own transport override; no
    // network connect, no topic listing.
    transport_stub(fetch);
  } else {
    // ---- connect (blocking; the client's own 30 s timeout bounds it) -------
    ConnectResult connect_result;
    fetch.connectFinished = [&connect_result](ConnectResult result) { connect_result = std::move(result); };
    fetch.connectAsync(target_uri, credentials);
    if (isCancelled(control)) {
      return {PJ_DESCRIPTOR_IMPORT_CANCELLED, "import cancelled"};
    }
    if (!connect_result.ok && !plaintext_retry_uri.empty()) {
      // The dialog's plaintext fallback (onConnectFinished): TLS failed and
      // this machine opted into insecure transport for the server.
      const std::string tls_error = connect_result.error;
      fetch.connectAsync(plaintext_retry_uri, plaintext_credentials);
      if (isCancelled(control)) {
        return {PJ_DESCRIPTOR_IMPORT_CANCELLED, "import cancelled"};
      }
      if (!connect_result.ok) {
        return {
            PJ_DESCRIPTOR_IMPORT_FAILED, "could not connect to " + target_uri + ": " + tls_error + "; nor to " +
                                             plaintext_retry_uri + ": " + connect_result.error};
      }
    } else if (!connect_result.ok) {
      return {PJ_DESCRIPTOR_IMPORT_FAILED, "could not connect to " + target_uri + ": " + connect_result.error};
    }

    // ---- topic list metadata -----------------------------------------------
    // Preserve size/timestamp context when the server supports listTopics.
    // pullTopicsAsync itself backfills every missing ontology tag, so
    // interactive and headless routing are identical by construction.
    fetch.listTopicsAsync(descriptor.sequence);
  }

  // ---- the pull (source record + whole-request completion ride the host
  // ingest bracket inside the fetch worker) ---------------------------------
  // Duration-ceiling watchdog: fires the FETCH cancel (like the byte ceiling)
  // so the terminal classifies as the ceiling, not caller cancel. ProviderJob
  // owns the watchdog through the whole body, whereas Mosaico's ceiling
  // covers transport only. The active gate preserves that scope: a later
  // expiry after the pull returned is inert.
  transfer_in_progress.store(true, std::memory_order_relaxed);
  control.armWatchdog(max_transfer_duration, [this] {
    if (!DescriptorImportProvider::claimTransferWatchdogExpiry(transfer_in_progress)) {
      return;
    }
    duration_ceiling_exceeded.store(true, std::memory_order_relaxed);
    fetch.requestCancel();
  });
  fetch.pullTopicsAsync(descriptor.sequence, descriptor.topics, descriptor.start_ns, descriptor.end_ns);
  transfer_in_progress.store(false, std::memory_order_relaxed);

  // ---- terminal mapping ----------------------------------------------------
  // Precedence (the pinned order): for a NON-COMPLETE pull, an explicit
  // caller cancel wins — even over a ceiling when both latched; the ceilings
  // then win over every other failure cause. A batch that COMPLETED before a
  // post-completion cancel reports its truthful terminal (the host's cancel
  // rollback is ledger-based either way).
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
  // A batch whose provisional dataset was rolled back (all requested windows
  // empty), or that never created one, has no dataset to hand the host:
  // SUCCEEDED_EAGER_ONLY would announce a dead handle to a layout restore.
  if (!dataset_created.load() || dataset_discarded.load()) {
    return {PJ_DESCRIPTOR_IMPORT_FAILED, "import produced no usable dataset (requested windows may be empty)"};
  }
  // The host owns caching/materialization in the M3 flow, so a completed
  // pull is always EAGER_ONLY from the provider's point of view: the source
  // record + whole-request completion the fetch attached through the ingest
  // bracket are what the host caches from.
  return {PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY, "import completed"};
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

    // Result strings: owned by this instance, valid until the NEXT query on
    // it (the ABI lifetime rule; main-thread only, like the query itself).
    query_result_ = PJ::DescriptorQueryResult{};
    query_result_.source_identity = descriptorIdentity(*descriptor);
    // The host builds its dialog rows only after this bounded query returns.
    // Publish the descriptor's human presentation now so first loads on a new
    // machine do not fall back to the opaque durable identity.
    PJ::sdk::source::recordSourcePresentation(
        settings_, query_result_.source_identity, {descriptor->display_name, descriptor->sequence, descriptor->origin});

    // Trust: strict origin equality against the process environment
    // allowlist, checked against the URI the headless import would actually
    // connect to (grpc+tls:// unless the per-machine settings opted this
    // origin into plaintext). v1 emits only trusted / needs-confirmation
    // (refused is reserved for future policy).
    const bool trusted = trustedByEnvironment(headlessTargetUri(descriptor->origin));
    query_result_.trust = trusted ? PJ::DescriptorTrust::kTrusted : PJ::DescriptorTrust::kNeedsConfirmation;
    if (!trusted) {
      query_result_.message = "server not trusted on this machine; confirm the download or set MOSAICO_TRUSTED_ORIGINS";
    }
    // The HOST owns the cache in the M3 flow: it classifies hit/miss against
    // its own store by identity, so the provider never reports a local
    // materialization or a size estimate.
    query_result_.is_materialized = false;

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
      // Unknown flag bits fail closed FIRST (the SDK reader rejects them
      // before anything else is looked at).
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

    // Credentials + transport scheme resolved ON THIS (main) thread —
    // SettingsView is main-thread-only; the job thread never touches the
    // view. The env-key origin guard lives in resolveHeadlessCredentials.
    auto state = std::make_shared<JobState>();
    state->bindings = bindings_;
    state->descriptor = *descriptor;
    state->target_uri = headlessTargetUri(descriptor->origin);
    state->credentials = resolveHeadlessCredentials(settings_, state->target_uri);
    if (const auto retry = headlessPlaintextRetryUri(settings_, descriptor->origin)) {
      state->plaintext_retry_uri = *retry;
      state->plaintext_credentials = resolveHeadlessCredentials(settings_, *retry);
    }
    // The provider's per-machine hard limits apply even when the caller
    // imposes none — effective = min-nonzero(caller, provider). Read on the
    // main thread, like every other start-time resolution.
    state->max_transfer_bytes = PJ::sdk::source::minNonzero(
        parsed_request->max_transfer_bytes,
        PJ::sdk::source::envLimit("MOSAICO_IMPORT_MAX_BYTES", kDefaultImportMaxBytes));
    // Cap before the seconds->ms conversion so a huge value saturates instead
    // of overflowing the int64 millisecond count.
    const std::uint64_t max_seconds = std::min<std::uint64_t>(
        PJ::sdk::source::envLimit("MOSAICO_IMPORT_MAX_SECONDS", kDefaultImportMaxSeconds),
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1000));
    state->max_transfer_duration = std::chrono::milliseconds(static_cast<std::int64_t>(max_seconds) * 1000);
    state->transport_stub = transport_stub_for_test_;
    auto body = [state](PJ::sdk::source::JobControl& control) { return state->runToTerminal(control); };
    return PJ::sdk::source::ProviderJob::start(std::move(body), callbacks, callback_ctx, out_job, out_error);
  } catch (...) {
    PJ::sdk::fillError(out_error, 1, "mosaico", "internal error in start_import");
    return false;
  }
}

}  // namespace mosaico
