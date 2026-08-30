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
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <pj_base/sdk/platform.hpp>
#include <semaphore>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "arrow_cache_artifact.hpp"
#include "core/origin_match.h"
#include "credential_resolve.hpp"
#include "fetch_worker.hpp"
#include "settings_store.hpp"
#include "source_descriptor.hpp"
#include "source_presentation.hpp"
#include "stoppable_thread.hpp"

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

std::string_view trimAsciiWhitespace(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return text;
}

bool trustedByEnvironment(std::string_view server_uri) {
  const auto target = parseGrpcOrigin(server_uri);
  const std::optional<std::string> allowlist = PJ::sdk::getEnv("MOSAICO_TRUSTED_ORIGINS");
  if (!target.has_value() || !allowlist.has_value()) {
    return false;
  }

  std::string_view remaining = *allowlist;
  for (;;) {
    const std::size_t comma = remaining.find(',');
    const std::string_view entry = trimAsciiWhitespace(remaining.substr(0, comma));
    if (const auto candidate = parseGrpcOrigin(entry); candidate.has_value() && candidate->scheme == target->scheme &&
                                                       candidate->host == target->host &&
                                                       candidate->port == target->port) {
      return true;
    }
    if (comma == std::string_view::npos) {
      return false;
    }
    remaining.remove_prefix(comma + 1);
  }
}

// Settled-exactly-once promotion outcome, shared (shared_ptr) between the
// FetchWorker's promotionSettled callback — which may fire re-entrantly, on
// any thread, or long after the pull returned — and the job's waiter. The
// callback captures ONLY this shared state, so a late settle can never touch
// freed memory. wait() checks `settled` BEFORE `cancelled`, so a settle that
// raced a post-completion cancel still reports truthfully.
struct PromotionLatch {
  std::mutex mu;
  std::condition_variable cv;
  bool settled = false;
  bool promoted = false;
  std::string detail;

  void settle(bool ok, std::string message) {
    {
      std::lock_guard<std::mutex> lock(mu);
      if (settled) {
        return;
      }
      settled = true;
      promoted = ok;
      detail = std::move(message);
    }
    cv.notify_all();
  }

  /// True when settled; false = `cancelled()` while still outstanding
  /// (DETACH: this shared state outlives the caller and settles whenever the
  /// promotion result fires).
  [[nodiscard]] bool wait(const std::function<bool()>& cancelled, std::chrono::milliseconds poll) {
    std::unique_lock<std::mutex> lock(mu);
    for (;;) {
      if (settled) {
        return true;
      }
      if (cancelled()) {
        return false;
      }
      cv.wait_for(lock, poll);
    }
  }

  [[nodiscard]] bool isPromoted() {
    std::lock_guard<std::mutex> lock(mu);
    return promoted;
  }
  [[nodiscard]] std::string detailCopy() {
    std::lock_guard<std::mutex> lock(mu);
    return detail;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// JobState — one import job: worker thread + a private FetchWorker + the ABI
// callbacks
// ---------------------------------------------------------------------------

struct DescriptorImportProvider::JobState {
  explicit JobState(SessionFileCache cache) : file_cache(std::move(cache)) {}

  // ---- immutable inputs (written by startImport BEFORE the worker starts) --
  HostBindings bindings;
  SourceDescriptor descriptor;
  std::string identity;
  ServerCredentials credentials;  // resolved on the main thread
  std::string cache_root_override;
  SessionFileCache file_cache;  // same root the tee will publish under
  // Hands the pull's promotion leases to the provider (which outlives this
  // job); set by startImport, invoked on the job thread after the pull.
  std::function<void(std::unordered_map<std::string, FileLock>)> adopt_leases;
  std::uint64_t max_transfer_bytes = 0;
  std::chrono::milliseconds max_transfer_duration{0};
  std::chrono::milliseconds poll{50};
  std::function<void()> pre_terminal_hook;  // test seam

  // Copied ABI callback pointers (the ABI: copied before start returns).
  void (*on_dataset)(void*, PJ_data_source_handle_t) noexcept = nullptr;
  void (*on_terminal)(void*, PJ_descriptor_import_outcome_t, PJ_string_view_t) noexcept = nullptr;
  void* callback_ctx = nullptr;

  // ---- lifecycle -----------------------------------------------------------
  FetchWorker fetch;  // per-job: owns the client + cancel wake machinery
  std::binary_semaphore start_gate{0};
  std::atomic<bool> start_released{false};
  std::atomic<bool> cancelled{false};
  std::atomic<int> terminal_count{0};
  std::thread worker;
  // Immutable after startImport (set before out_job is returned): the
  // worker's id for the self-join guard — reading worker.get_id() during a
  // concurrent join() would race the join's modification of the thread
  // object.
  std::thread::id worker_id;
  // join()-state: the ABI slot is [thread-safe]/idempotent, but
  // std::thread::join is neither — exactly ONE caller performs the join,
  // every other caller waits for `joined`; all errors are swallowed inside
  // the noexcept boundary.
  std::mutex join_mu;
  std::condition_variable join_cv;
  bool join_in_progress = false;
  bool joined = false;

  // ---- per-run result state ------------------------------------------------
  std::shared_ptr<PromotionLatch> promotion_latch = std::make_shared<PromotionLatch>();
  std::atomic<bool> dataset_created{false};
  std::atomic<bool> imported_any{false};
  std::atomic<bool> any_failed{false};
  std::atomic<bool> byte_ceiling_exceeded{false};
  std::atomic<bool> duration_ceiling_exceeded{false};
  std::mutex results_mu;
  std::string first_error;                             // guarded by results_mu
  std::map<std::string, std::int64_t> bytes_by_topic;  // guarded by results_mu
  std::uint64_t total_bytes = 0;                       // guarded by results_mu

  // At-most-once gate release (std::binary_semaphore::release on an
  // already-released semaphore is UB) — callable from startImport (the
  // normal path) and defensively from destroy.
  void releaseStartOnce() {
    bool expected = false;
    if (start_released.compare_exchange_strong(expected, true)) {
      start_gate.release();
    }
  }

  // [thread-safe] Idempotent, non-blocking: flag + the pull's cancel wake
  // machinery (Flight reader interrupts) — the poll loops read the flag.
  void requestCancel() {
    cancelled.store(true, std::memory_order_relaxed);
    fetch.requestCancel();
  }

  [[nodiscard]] bool isCancelled() const {
    return cancelled.load(std::memory_order_relaxed);
  }

  // Exactly-once terminal (defensive counter; the run() flow fires it once).
  void fireTerminal(PJ_descriptor_import_outcome_t outcome, const std::string& message) {
    if (terminal_count.fetch_add(1) != 0) {
      return;
    }
    if (on_terminal != nullptr) {
      on_terminal(callback_ctx, outcome, PJ_string_view_t{message.data(), message.size()});
    }
  }

  void run() noexcept;
  PJ_descriptor_import_outcome_t runToTerminal(std::string* message);
};

void DescriptorImportProvider::JobState::run() noexcept {
  // FIRST action: block on the start gate — the ABI forbids any job callback
  // before start_import returns; startImport releases the gate as it is
  // about to return, after out_job is fully populated.
  start_gate.acquire();
  PJ_descriptor_import_outcome_t outcome = PJ_DESCRIPTOR_IMPORT_FAILED;
  std::string message;
  try {
    outcome = runToTerminal(&message);
  } catch (...) {
    outcome = PJ_DESCRIPTOR_IMPORT_FAILED;
    message = "internal error while running the import";
  }
  fireTerminal(outcome, message);
}

PJ_descriptor_import_outcome_t DescriptorImportProvider::JobState::runToTerminal(std::string* message) {
  if (isCancelled()) {
    *message = "import cancelled";
    return PJ_DESCRIPTOR_IMPORT_CANCELLED;
  }

  // The host classifies hit/miss BEFORE starting a job, so a fresh VALID
  // cache file here means a concurrent materialization won the race (the
  // interactive fetch, or another process). No eager ingest ran here (ZERO
  // on_dataset), so the only truthful terminal is a retry classification —
  // never promote, never report success (the mcap_cloud provider's locked v1
  // decision). Ongoing (unfinished) materializations are arbitrated later by
  // the tee's materialize lock: the tee simply disarms and this import lands
  // eager-only.
  {
    std::filesystem::path disk;
    if (file_cache.lookup(identity, &disk)) {
      *message = "session was materialized concurrently; reload to classify it as a cache hit";
      return PJ_DESCRIPTOR_IMPORT_FAILED;
    }
  }

  fetch.setHostProvider(bindings.host_provider);
  fetch.setRuntimeHostProvider(bindings.runtime_host_provider);
  fetch.setPromotionProvider(bindings.promotion_provider);

  // on_dataset: zero-or-one, fired when the pull creates its batch dataset,
  // strictly before any publication into it.
  fetch.datasetCreated = [this](PJ::sdk::DataSourceHandle handle) {
    if (!dataset_created.exchange(true)) {
      if (on_dataset != nullptr) {
        on_dataset(callback_ctx, PJ_data_source_handle_t{handle.id});
      }
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
  fetch.hostStopRequested = [this] { cancelled.store(true, std::memory_order_relaxed); };
  // Promotion settlement rides the shared latch (see PromotionLatch).
  {
    auto latch = promotion_latch;
    fetch.promotionSettled = [latch](bool ok, std::string detail) { latch->settle(ok, std::move(detail)); };
  }

  // ---- connect (blocking; the client's own 30 s timeout bounds it) ---------
  ConnectResult connect_result;
  fetch.connectFinished = [&connect_result](ConnectResult result) { connect_result = std::move(result); };
  fetch.connectAsync(descriptor.server_uri, credentials);
  if (isCancelled()) {
    *message = "import cancelled";
    return PJ_DESCRIPTOR_IMPORT_CANCELLED;
  }
  if (!connect_result.ok) {
    *message = "could not connect to " + descriptor.server_uri + ": " + connect_result.error;
    return PJ_DESCRIPTOR_IMPORT_FAILED;
  }

  // ---- per-topic metadata (ontology routing) -------------------------------
  // Mirror the interactive flow: listTopics fills the size/timestamp cache,
  // then getTopicMetadata refreshes each selected topic with its ontology
  // tag. Failures are non-fatal — the pulled stream's own schema metadata is
  // the routing fallback.
  fetch.listTopicsAsync(descriptor.sequence);
  for (const auto& topic : descriptor.topics) {
    if (isCancelled()) {
      *message = "import cancelled";
      return PJ_DESCRIPTOR_IMPORT_CANCELLED;
    }
    fetch.fetchTopicMetadataAsync({descriptor.sequence, topic});
  }

  // ---- the descriptor-armed pull (cache tee + promotion) -------------------
  {
    // Duration-ceiling watchdog: fires the FETCH cancel (like the byte
    // ceiling) so the terminal classifies as the ceiling. Scoped to the
    // pull; the destructor stops + joins on every exit path.
    std::optional<StoppableThread> watchdog;
    if (max_transfer_duration.count() > 0) {
      watchdog.emplace([this](StoppableThread& thread) {
        if (!thread.waitForStop(max_transfer_duration)) {
          duration_ceiling_exceeded.store(true, std::memory_order_relaxed);
          fetch.requestCancel();
        }
      });
    }
    fetch.pullTopicsAsync(
        descriptor.sequence, descriptor.topics, descriptor.start_ns, descriptor.end_ns, descriptor,
        cache_root_override);
  }
  // The promoted dataset lazily re-opens the artifact long after this job
  // dies — move the pull's read leases to the provider before the terminal.
  if (adopt_leases) {
    adopt_leases(fetch.takeArtifactLeases());
  }
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
  if (isCancelled() && !batch_complete) {
    *message = "import cancelled";
    return PJ_DESCRIPTOR_IMPORT_CANCELLED;
  }
  if (byte_ceiling_exceeded.load()) {
    *message = "import exceeded the transfer byte ceiling (MOSAICO_IMPORT_MAX_BYTES)";
    return PJ_DESCRIPTOR_IMPORT_FAILED;
  }
  if (duration_ceiling_exceeded.load()) {
    *message = "import exceeded the transfer time ceiling (MOSAICO_IMPORT_MAX_SECONDS)";
    return PJ_DESCRIPTOR_IMPORT_FAILED;
  }
  if (!imported_any.load()) {
    std::lock_guard<std::mutex> lock(results_mu);
    *message = first_error.empty() ? "no topic could be imported" : first_error;
    return PJ_DESCRIPTOR_IMPORT_FAILED;
  }

  // ---- promotion settlement (cancel-aware, DETACH on cancel) ---------------
  // The FetchWorker fires promotionSettled on every armed-tee path; an
  // ACCEPTED promotion settles whenever the host's result callback runs —
  // possibly after the pull returned. A cancel while it is outstanding
  // detaches: the shared latch outlives this job and join() stays
  // unblockable.
  const bool settled = promotion_latch->wait([this] { return isCancelled(); }, poll);
  if (!settled) {
    *message = "import cancelled (promotion still pending; detached)";
    return PJ_DESCRIPTOR_IMPORT_CANCELLED;
  }
  const bool eager_usable = dataset_created.load() && imported_any.load();
  if (promotion_latch->isPromoted()) {
    *message = "promoted to a file-backed source";
    return PJ_DESCRIPTOR_IMPORT_SUCCEEDED_PROMOTED;
  }
  if (eager_usable) {
    *message = "import completed without promotion (" + promotion_latch->detailCopy() + ")";
    return PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY;
  }
  *message = "import produced no usable dataset (" + promotion_latch->detailCopy() + ")";
  return PJ_DESCRIPTOR_IMPORT_FAILED;
}

// ---------------------------------------------------------------------------
// The job vtable trio
// ---------------------------------------------------------------------------

void DescriptorImportProvider::jobCancel(void* ctx) noexcept {
  if (ctx != nullptr) {
    static_cast<JobState*>(ctx)->requestCancel();
  }
}

void DescriptorImportProvider::jobJoin(void* ctx) noexcept {
  if (ctx == nullptr) {
    return;
  }
  auto* state = static_cast<JobState*>(ctx);
  // ABI: join/destroy must never be called from a job callback (that thread
  // joining itself is deadlock-or-terminate). The host is trusted; this
  // guard documents the rule and downgrades a violation to a no-op. The id
  // read is the IMMUTABLE worker_id member — never worker.get_id(), which
  // would race a concurrent join()'s modification of the thread object.
  if (state->worker_id == std::this_thread::get_id()) {
    return;
  }
  // Concurrent-caller safety: two allowed non-callback threads may call
  // join() together, but std::thread::join is not thread-safe — one caller
  // performs the join, the rest wait for `joined`, and EVERY error stays
  // inside this noexcept boundary.
  try {
    std::unique_lock<std::mutex> lock(state->join_mu);
    if (state->joined) {
      return;
    }
    if (state->join_in_progress) {
      state->join_cv.wait(lock, [state] { return state->joined; });
      return;
    }
    state->join_in_progress = true;
    lock.unlock();
    try {
      if (state->worker.joinable()) {
        state->worker.join();
      }
    } catch (...) {
      // Swallow: the thread either finished or the join failed; either way
      // the terminal contract is the run() flow's, not join()'s.
    }
    lock.lock();
    state->joined = true;
    state->join_cv.notify_all();
  } catch (...) {
    // Lock/wait failure: nothing safe left to do inside noexcept.
  }
}

void DescriptorImportProvider::jobDestroy(void* ctx) noexcept {
  if (ctx == nullptr) {
    return;
  }
  // NOTE: jobJoin's self-join guard does NOT make destroy-from-a-callback
  // survivable — the guard only skips the join, and `delete state` below
  // then destroys a still-joinable std::thread member, which is
  // std::terminate. The ABI's "never call join/destroy from a job callback"
  // rule is load-bearing here, not merely advisory.
  auto* state = static_cast<JobState*>(ctx);
  jobCancel(ctx);
  // Defensive: startImport always releases the gate before returning, but a
  // gate that was somehow never released must not deadlock the join below.
  state->releaseStartOnce();
  jobJoin(ctx);
  delete state;
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

SessionFileCache DescriptorImportProvider::makeFileCache() {
  const std::string configured = SettingsStore(settings_).getString("mosaico/cache_directory");
  return SessionFileCache::at(std::filesystem::path(configured), validateArtifact, nullptr);
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

    SessionFileCache cache = makeFileCache();
    // Result strings: owned by this instance, valid until the NEXT query on
    // it (the ABI lifetime rule; main-thread only, like the query itself).
    query_identity_ = descriptorIdentity(*descriptor);
    // The host builds the dialog rows only after this bounded query returns.
    // Publish the descriptor's human presentation now so first loads on a new
    // machine do not fall back to the opaque durable identity.
    recordSourcePresentation(settings_, query_identity_, *descriptor);
    query_path_ = cache.pathFor(query_identity_).string();
    query_message_.clear();

    // Trust: strict origin equality against the process environment
    // allowlist. Malformed entries are ignored; v1 emits only trusted /
    // needs-confirmation (refused is reserved for future policy).
    const bool trusted = trustedByEnvironment(descriptor->server_uri);
    if (!trusted) {
      query_message_ = "server not trusted on this machine; confirm the download or set MOSAICO_TRUSTED_ORIGINS";
    }

    // is_materialized: the DISK-validated cache ONLY, with lease-then-
    // validate pinning — a materialized hit is a path the HOST is about to
    // load, and after a process restart no finalize-time lease exists, so
    // the query itself pins it (idempotent via the lease map). On lease
    // contention (another process holds the exclusive) we still answer
    // honestly from a plain validation, just without retention.
    bool materialized = false;
    std::filesystem::path disk;
    if (const auto held = query_leases_.find(query_identity_); held != query_leases_.end()) {
      materialized = cache.lookup(query_identity_, &disk);
      if (!materialized) {
        query_leases_.erase(held);  // vanished/corrupt under our pin: drop it
      }
    } else {
      std::string lease_error;
      if (auto lease = cache.acquireReadLease(query_identity_, &lease_error)) {
        materialized = cache.lookup(query_identity_, &disk);
        if (materialized) {
          query_leases_.emplace(query_identity_, std::move(*lease));
        }
      } else {
        materialized = cache.lookup(query_identity_, &disk);
        if (materialized && query_message_.empty()) {
          query_message_ = "cache artifact present but its read lease is contended";
        }
      }
    }
    std::uint64_t estimated_bytes = 0;
    if (materialized) {
      std::error_code ec;
      const auto size = std::filesystem::file_size(disk, ec);
      if (!ec) {
        estimated_bytes = static_cast<std::uint64_t>(size);
      }
    }

    // Growth contract: write ONLY fields wholly covered by the caller's
    // struct_size.
    auto covered = [out_result](std::size_t offset, std::size_t size) {
      return out_result->struct_size >= offset + size;
    };
    if (covered(offsetof(PJ_descriptor_query_result_v1_t, trust), sizeof(out_result->trust))) {
      out_result->trust = trusted ? PJ_DESCRIPTOR_TRUST_TRUSTED : PJ_DESCRIPTOR_TRUST_NEEDS_CONFIRMATION;
    }
    if (covered(offsetof(PJ_descriptor_query_result_v1_t, is_materialized), sizeof(out_result->is_materialized))) {
      out_result->is_materialized = materialized ? 1 : 0;
    }
    if (covered(offsetof(PJ_descriptor_query_result_v1_t, source_identity), sizeof(out_result->source_identity))) {
      out_result->source_identity = PJ_string_view_t{query_identity_.data(), query_identity_.size()};
    }
    if (covered(offsetof(PJ_descriptor_query_result_v1_t, local_path_utf8), sizeof(out_result->local_path_utf8))) {
      out_result->local_path_utf8 = PJ_string_view_t{query_path_.data(), query_path_.size()};
    }
    if (covered(offsetof(PJ_descriptor_query_result_v1_t, message), sizeof(out_result->message))) {
      out_result->message = PJ_string_view_t{query_message_.data(), query_message_.size()};
    }
    if (covered(offsetof(PJ_descriptor_query_result_v1_t, estimated_bytes), sizeof(out_result->estimated_bytes))) {
      out_result->estimated_bytes = estimated_bytes;
    }
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
    if (request == nullptr || out_job == nullptr) {
      PJ::sdk::fillError(out_error, 1, "mosaico", "null request/out_job");
      return false;
    }
    auto req_covered = [request](std::size_t offset, std::size_t size) {
      return request->struct_size >= offset + size;
    };

    // FLAGS FAIL CLOSED, FIRST: unknown bits reject synchronously — no
    // callbacks, out_job untouched (the ABI's fail-closed spine).
    const std::uint64_t flags =
        req_covered(offsetof(PJ_descriptor_import_start_request_v1_t, flags), sizeof(request->flags))
            ? request->flags
            : PJ_DESCRIPTOR_IMPORT_START_FLAG_NONE;
    if ((flags & ~PJ_DESCRIPTOR_IMPORT_START_FLAGS_V1_MASK) != 0) {
      PJ::sdk::fillError(out_error, 1, "mosaico", "unknown start_import flag bits (fail closed)");
      return false;
    }

    // Required callback surface: on_terminal is the exactly-once spine.
    const bool terminal_covered =
        callbacks != nullptr && callbacks->struct_size >= offsetof(PJ_descriptor_import_callbacks_v1_t, on_terminal) +
                                                              sizeof(callbacks->on_terminal);
    if (!terminal_covered || callbacks->on_terminal == nullptr) {
      PJ::sdk::fillError(out_error, 1, "mosaico", "on_terminal callback is required");
      return false;
    }

    std::string parse_error;
    const std::string_view descriptor_json =
        req_covered(
            offsetof(PJ_descriptor_import_start_request_v1_t, descriptor_json), sizeof(request->descriptor_json))
            ? toView(request->descriptor_json)
            : std::string_view{};
    const std::optional<SourceDescriptor> descriptor = parseSourceDescriptor(descriptor_json, &parse_error);
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
    auto* state = new JobState(makeFileCache());
    state->bindings = bindings_;
    state->descriptor = *descriptor;
    state->identity = descriptorIdentity(*descriptor);
    state->credentials = resolveHeadlessCredentials(settings_, descriptor->server_uri);
    state->cache_root_override = SettingsStore(settings_).getString("mosaico/cache_directory");
    const std::uint64_t caller_bytes =
        req_covered(
            offsetof(PJ_descriptor_import_start_request_v1_t, max_transfer_bytes), sizeof(request->max_transfer_bytes))
            ? request->max_transfer_bytes
            : 0;
    // The provider's per-machine hard limits apply even when the caller
    // imposes none — effective = min-nonzero(caller, provider). Read on the
    // main thread, like every other start-time resolution.
    state->max_transfer_bytes = minNonzero(caller_bytes, envLimit("MOSAICO_IMPORT_MAX_BYTES", kDefaultImportMaxBytes));
    state->max_transfer_duration =
        std::chrono::milliseconds(envLimit("MOSAICO_IMPORT_MAX_SECONDS", kDefaultImportMaxSeconds) * 1000);
    state->poll = poll_;
    state->pre_terminal_hook = pre_terminal_hook_;
    state->adopt_leases = [this](std::unordered_map<std::string, FileLock> leases) {
      std::lock_guard<std::mutex> lock(job_leases_mu_);
      for (auto& [identity, lease] : leases) {
        job_leases_.insert_or_assign(identity, std::move(lease));
      }
    };
    state->on_dataset = callbacks->on_dataset;  // may be null (zero-or-one)
    state->on_terminal = callbacks->on_terminal;
    state->callback_ctx = callback_ctx;

    // Spawn the GATED worker first (its first action is start_gate.acquire(),
    // so it cannot touch anything before the release below): a thread-spawn
    // failure must return false with out_job UNTOUCHED and no leaked state.
    try {
      state->worker = std::thread([state]() { state->run(); });
    } catch (...) {
      delete state;
      PJ::sdk::fillError(out_error, 1, "mosaico", "could not start the import worker thread");
      return false;
    }
    state->worker_id = state->worker.get_id();  // immutable from here

    // Populate out_job with the worker safely gated; the caller reads it
    // only after this returns.
    out_job->ctx = state;
    out_job->vtable = &kJobVtable;

    // The explicit post-return START GATE: released only now — after out_job
    // is fully populated and this thunk is about to return — so the worker's
    // FIRST action cannot proceed earlier. (The residual caller-side window
    // between this release and the caller resuming is the ABI's known,
    // callee-unfixable gap; the SDK's reference provider has the identical
    // shape.)
    if (start_gate_probe_) {
      start_gate_probe_();
    }
    state->releaseStartOnce();
    return true;
  } catch (...) {
    PJ::sdk::fillError(out_error, 1, "mosaico", "internal error in start_import");
    return false;
  }
}

}  // namespace mosaico
