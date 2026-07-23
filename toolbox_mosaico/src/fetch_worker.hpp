// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

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

  void requestCancel() {
    cancel_flag_.store(true, std::memory_order_relaxed);
  }
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
  /// The HOST asked this download to stop (title-bar Stop → progress_update
  /// returning false). requestCancel() has already been called when this
  /// fires; the dialog uses it to flip its own "Cancelling…" UI state exactly
  /// as if the in-panel Cancel button had been pressed. Fired at most once per
  /// Download, from an SDK pool thread.
  std::function<void()> hostStopRequested;

 private:
  /// Return the single DataSourceHandle for the current Download, creating it
  /// on first use. All topics of one sequence share this handle so they land
  /// in ONE catalog group; the mutex makes the lazy-create safe under the
  /// parallel per-topic callbacks of pullTopicsAsync. Each Download entry
  /// point resets fetch_dataset_ to std::nullopt before its per-topic loop.
  [[nodiscard]] PJ::Expected<PJ::sdk::DataSourceHandle> datasetForFetch(
      const PJ::sdk::ToolboxHostView& host, const std::string& sequence_name);

  /// Create the host progress/stop channel for this Download, once per batch:
  /// a parser-ingest context bound to @p ds (tail-slot-gated; a host without
  /// the slots, or without parser-ingest configured, degrades to no progress).
  /// On success stores the fat-pointer view + calls progressStart
  /// (indeterminate — see the .cpp). Runs at the FIRST topic's on_done because
  /// the dataset must exist first (lazy datasetForFetch): a single-topic
  /// Download therefore gets only the started/finished bracket, and host Stop
  /// becomes available from the first completed topic — the in-panel Cancel
  /// covers the window before that. Caller holds host_write_mu_; takes
  /// progress_mu_.
  void ensureIngestProgress(const PJ::sdk::DataSourceHandle& ds, const std::string& sequence_name);

  /// End-of-batch bracket: progressFinish + releaseParserIngest (idempotent,
  /// [thread-safe]). Releasing BEFORE allFetchesComplete is load-bearing: the
  /// dialog's terminal notifyDataChanged is queued after it, so the host's
  /// release-time report focuses playback on the finished dataset.
  void finishIngestProgress();

  std::unique_ptr<MosaicoClient> client_;
  std::function<PJ::sdk::ToolboxHostView()> host_provider_;
  std::function<PJ::ToolboxRuntimeHostView()> runtime_host_provider_;
  std::atomic<bool> cancel_flag_{false};
  std::unordered_map<std::string, TopicInfo> topic_info_by_name_;
  std::optional<PJ::sdk::DataSourceHandle> fetch_dataset_;
  std::mutex fetch_dataset_mu_;
  // Host progress/stop channel state, all guarded by progress_mu_ — which also
  // SERIALIZES every call into the ingest fat pointer (progress_update is a
  // single-caller surface, but Mosaico progress events arrive on concurrent
  // SDK pool threads). Leaf-ish mutex: acquired under host_write_mu_ only in
  // ensureIngestProgress; the per-event tick takes it alone, so the only lock
  // ever nested inside it is the host's internal engine lock (also taken under
  // host_write_mu_ on the write path — no cycle).
  std::mutex progress_mu_;
  std::optional<PJ::DataSourceRuntimeHostView> ingest_progress_;
  bool ingest_progress_attempted_ = false;  // one create attempt per Download
  bool host_stop_reported_ = false;         // hostStopRequested fires at most once
  std::optional<uint32_t> ingest_ds_id_;
  // Cumulative decoded bytes per topic. Recorded on EVERY progress event, even
  // before the ingest context exists, so a late-created context's first sum is
  // complete.
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
