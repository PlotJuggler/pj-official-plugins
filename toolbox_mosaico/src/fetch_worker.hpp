// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <arrow/record_batch.h>
#include <arrow/result.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <pj_base/sdk/data_source_host_views.hpp>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/toolbox_plugin_base.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "flight/mosaico_client.hpp"
#include "flight/types.hpp"
#include "worker_types.h"

namespace mosaico {

namespace testing {
class FetchWorkerTestAccess;
}

/// Thin background adapter for MosaicoClient running on the dialog's worker
/// thread. The worker itself is transport-agnostic: callers serialize commands
/// and route callbacks to the GUI thread.
class FetchWorker {
 public:
  FetchWorker();
  ~FetchWorker();

  /// Provide a callback that returns the toolbox host. Set once on the GUI
  /// thread; reads happen on the worker thread, so the callback must be
  /// thread-safe or always return a stable view.
  void setHostProvider(std::function<PJ::sdk::ToolboxHostView()> provider) {
    host_provider_ = std::move(provider);
  }

  /// Provide the toolbox runtime host (progress + parser-ingest tail slots).
  /// Same contract as setHostProvider: set once at bind, read from worker/pool
  /// threads, so the callback must be thread-safe or return a stable view.
  /// Optional — when unset (or the host predates the tail slots), Downloads run
  /// exactly as before, just without host-side progress/stop integration.
  void setRuntimeHostProvider(std::function<PJ::ToolboxRuntimeHostView()> provider) {
    runtime_host_provider_ = std::move(provider);
  }

  /// Cancel the batch flag and actively interrupt any Flight readers that may
  /// currently be blocked in GetSchema/Next.
  void requestCancel();
  void resetCancel() {
    cancel_flag_.store(false, std::memory_order_relaxed);
  }
  [[nodiscard]] bool isCancelled() const {
    return cancel_flag_.load(std::memory_order_relaxed);
  }

  /// Cache topic infos from the most recent listTopicsAsync so per-topic
  /// ontology tags (used by image-data routing) can be looked up later
  /// without another round trip to the server. Keyed by topic_name.
  void setTopicInfoCache(std::unordered_map<std::string, TopicInfo> by_name) {
    topic_info_by_name_ = std::move(by_name);
  }

  /// Connect (or reconnect) to the given URI with the per-server @p creds. Calls
  /// connectFinished on completion.
  void connectAsync(std::string uri, ServerCredentials creds);

  /// List all sequences from the connected server.
  void listSequencesAsync();

  /// List topics for a given sequence (partial metadata).
  void listTopicsAsync(std::string sequence_name);

  /// Fetch full per-topic metadata (Arrow schema, ontology tag, user
  /// metadata) on demand for the Info panel. Calls topicMetadataReady.
  void fetchTopicMetadataAsync(TopicRef topic);

  /// Pull several topics of the same sequence in parallel via the SDK's
  /// connection pool. Each topic emits its own pullProgress / pullFinished
  /// callback; the per-topic host-write section is serialized by host_write_mu_
  /// so the plugin's bookkeeping is safe regardless of SDK-internal locking.
  /// After all topics complete, allFetchesComplete fires once.
  void pullTopicsAsync(
      std::string sequence_name, std::vector<std::string> topic_names, std::int64_t start_ns, std::int64_t end_ns);

  std::function<void(ConnectResult result)> connectFinished;
  /// Full SequenceInfo entries, including user_metadata (used by the Lua
  /// metadata filter). The name-only callback is kept
  /// for code paths that only want names.
  std::function<void(std::vector<SequenceInfo> sequences)> sequencesReady;
  std::function<void(std::vector<std::string> names)> sequenceNamesReady;
  // Progressive discovery (PJ3 parity): the initial list, so the table can
  // populate before per-sequence detail finishes streaming in...
  std::function<void(std::vector<SequenceInfo> sequences)> sequenceListStarted;
  // ...and one callback per sequence as the server fills in its detail
  // (min/max timestamp, size, metadata), so Date/Size columns populate
  // incrementally rather than snapping in all at once.
  std::function<void(SequenceInfo sequence)> sequenceInfoReady;
  std::function<void(std::string sequence_name, std::vector<std::string> topic_names)> topicsReady;
  /// Full TopicInfo list from listTopics (name + size + timestamp range +
  /// created/locked/chunks). Schema/ontology/user_metadata are NOT populated
  /// here — those arrive via topicMetadataReady after fetchTopicMetadataAsync.
  std::function<void(std::string sequence_name, std::vector<TopicInfo> topics)> topicInfosReady;
  /// Full per-topic metadata (incl. Arrow schema) for the Info panel.
  std::function<void(TopicRef topic, TopicInfo info)> topicMetadataReady;
  std::function<void(std::string topic_name, std::int64_t bytes)> pullProgress;
  std::function<void(PullResultEvent result)> pullFinished;
  std::function<void(std::string sequence_name)> allFetchesComplete;
  /// Surfaced for non-fatal RPC failures that don't map to a topic/pull
  /// callback (e.g. listTopics returning a real server error rather than
  /// NotImplemented). Routed to the app notification bell (PJ3 parity:
  /// fetch_worker.cpp:105-117 emits errorOccurred for these statuses).
  std::function<void(std::string message)> errorOccurred;
  /// The HOST asked this download to stop (title-bar Stop → the ingest Stop
  /// flag, or progress_update returning false). requestCancel() has already
  /// been called when this fires; the dialog uses it to flip its own
  /// "Cancelling…" UI state exactly as if the in-panel Cancel button had been
  /// pressed. Fired at most once per Download, from a background thread.
  std::function<void()> hostStopRequested;

 private:
  friend class testing::FetchWorkerTestAccess;

  /// Return the single DataSourceHandle for the current Download, creating it
  /// on first use. A rollback-capable runtime creates it before transport for
  /// immediate host progress; older runtimes create it on the first importable
  /// topic. All completion callbacks share the handle so they land in ONE
  /// catalog group. Each Download resets fetch_dataset_ first.
  [[nodiscard]] PJ::Expected<PJ::sdk::DataSourceHandle> datasetForFetch(
      const PJ::sdk::ToolboxHostView& host, const std::string& sequence_name);

  /// Create the host progress/stop channel for this Download, once per batch:
  /// a parser-ingest context bound to @p ds (tail-slot-gated; a host without
  /// the slots, or without parser-ingest configured, degrades to no progress).
  /// On success stores the fat-pointer view + calls progressStart
  /// (indeterminate — see the .cpp). Runs after the batch dataset is created
  /// and before its transport starts. Caller holds host_write_mu_; takes
  /// progress_mu_.
  void ensureIngestProgress(const PJ::sdk::DataSourceHandle& ds, const std::string& sequence_name);

  /// Sum the per-topic byte ledger. Caller holds progress_mu_.
  [[nodiscard]] std::uint64_t accumulatedProgressBytesLocked() const;

  /// Read the ingest context's thread-safe host Stop flag.
  [[nodiscard]] bool isStopRequestedByHost();

  /// Route a host Stop through cancellation and the once-only UI callback.
  void requestCancelFromHost();

  /// Interrupt SDK readers (or the injected transport cancellation seam in a
  /// focused test). Safe when no client/pull is active.
  void cancelActivePulls();

  /// True only when the runtime host can roll back an eager provisional data
  /// source. Older hosts keep the original lazy-on-first-importable-topic
  /// behavior.
  [[nodiscard]] bool supportsProvisionalIngestDiscard() const;

  /// Invoke the optional discard tail slot across both the released SDK view
  /// and its ABI-compatible older headers.
  [[nodiscard]] bool discardProvisionalIngest(
      const PJ::ToolboxRuntimeHostView& runtime, std::uint32_t data_source_id) const;

  /// End-of-batch bracket: progressFinish + releaseParserIngest, or the
  /// provisional discard path for a zero-success batch. Finishing BEFORE
  /// allFetchesComplete is load-bearing: the dialog's terminal
  /// notifyDataChanged is queued after it, so the host's release-time report
  /// focuses playback on the finished dataset.
  void finishIngestProgress(bool discard_provisional);

  std::unique_ptr<MosaicoClient> client_;
  std::function<void()> pull_topics_override_;
  // Test seam: while pull_topics_override_ runs, the pull's per-batch and
  // per-topic-done callbacks (what the Flight client would invoke) so a test can
  // feed record batches through the ingest path without a server.
  std::function<void(const std::string&, const std::shared_ptr<arrow::RecordBatch>&)> on_batch_for_test_;
  std::function<void(const std::string&, arrow::Result<PullResult>)> on_done_for_test_;
  std::function<void()> cancel_active_pulls_override_;
  std::function<PJ::sdk::ToolboxHostView()> host_provider_;
  std::function<PJ::ToolboxRuntimeHostView()> runtime_host_provider_;
  std::atomic<bool> cancel_flag_{false};
  std::unordered_map<std::string, TopicInfo> topic_info_by_name_;
  std::optional<PJ::sdk::DataSourceHandle> fetch_dataset_;
  std::mutex fetch_dataset_mu_;
  // Host progress/stop channel state, all guarded by progress_mu_ — which also
  // SERIALIZES calls into the ingest fat pointer. Leaf-ish mutex: acquired
  // under host_write_mu_ only in ensureIngestProgress; progress and Stop polls
  // take it alone, so the only lock ever nested inside it is the host's internal
  // engine lock (also taken under host_write_mu_ on the write path — no cycle).
  std::mutex progress_mu_;
  std::optional<PJ::DataSourceRuntimeHostView> ingest_progress_;
  bool ingest_progress_attempted_ = false;  // one create attempt per Download
  bool host_stop_reported_ = false;         // hostStopRequested fires at most once
  std::optional<uint32_t> ingest_ds_id_;
  // Cumulative decoded bytes per topic, published as a batch-wide sum.
  std::map<std::string, std::int64_t, std::less<>> progress_bytes_by_topic_;
  // [C1] Serializes the ENTIRE host-write critical section in pullTopicsAsync's
  // per-topic on_done callback (datasetForFetch + register/append/push), which
  // runs on SDK connection-pool worker threads against a host DataWriter that
  // has no internal mutex. Self-owned: the plugin no longer relies on the SDK
  // serializing on_done. Lock order is always host_write_mu_ -> fetch_dataset_mu_
  // (the latter taken inside datasetForFetch), never the reverse.
  std::mutex host_write_mu_;
};

}  // namespace mosaico
