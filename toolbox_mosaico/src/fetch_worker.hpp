// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "flight/mosaico_client.hpp"
#include "flight/types.hpp"

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

  /// Connect (or reconnect) to the given URI. Calls connectFinished on completion.
  void connectAsync(std::string uri, std::string cert_path, std::string api_key, bool allow_insecure);

  /// List all sequences from the connected server.
  void listSequencesAsync();

  /// List topics for a given sequence (partial metadata).
  void listTopicsAsync(std::string sequence_name);

  /// Fetch full per-topic metadata (Arrow schema, ontology tag, user
  /// metadata) on demand for the Info panel. Calls topicMetadataReady.
  void fetchTopicMetadataAsync(std::string sequence_name, std::string topic_name);

  /// Pull several topics of the same sequence in parallel via the SDK's
  /// connection pool. Each topic emits its own pullProgress / pullFinished
  /// callback; the per-topic host-write section is serialized by host_write_mu_
  /// so the plugin's bookkeeping is safe regardless of SDK-internal locking.
  /// After all topics complete, allFetchesComplete fires once.
  void pullTopicsAsync(
      std::string sequence_name, std::vector<std::string> topic_names, std::int64_t start_ns, std::int64_t end_ns);

  std::function<void(bool ok, std::string status, std::string error)> connectFinished;
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
  std::function<void(std::string sequence_name, std::string topic_name, TopicInfo info)> topicMetadataReady;
  std::function<void(std::string topic_name, std::int64_t bytes)> pullProgress;
  std::function<void(std::string sequence_name, std::string topic_name, bool ok, std::string error)> pullFinished;
  std::function<void(std::string sequence_name)> allFetchesComplete;
  /// Surfaced for non-fatal RPC failures that don't map to a topic/pull
  /// callback (e.g. listTopics returning a real server error rather than
  /// NotImplemented). Routed to the app notification bell (PJ3 parity:
  /// fetch_worker.cpp:105-117 emits errorOccurred for these statuses).
  std::function<void(std::string message)> errorOccurred;

 private:
  /// Return the single DataSourceHandle for the current Download, creating it
  /// on first use. All topics of one sequence share this handle so they land
  /// in ONE catalog group; the mutex makes the lazy-create safe under the
  /// parallel per-topic callbacks of pullTopicsAsync. Each Download entry
  /// point resets fetch_dataset_ to std::nullopt before its per-topic loop.
  [[nodiscard]] PJ::Expected<PJ::sdk::DataSourceHandle> datasetForFetch(
      const PJ::sdk::ToolboxHostView& host, const std::string& sequence_name);

  std::unique_ptr<MosaicoClient> client_;
  std::function<PJ::sdk::ToolboxHostView()> host_provider_;
  std::atomic<bool> cancel_flag_{false};
  std::unordered_map<std::string, TopicInfo> topic_info_by_name_;
  std::optional<PJ::sdk::DataSourceHandle> fetch_dataset_;
  std::mutex fetch_dataset_mu_;
  // [C1] Serializes the ENTIRE host-write critical section in pullTopicsAsync's
  // per-topic on_done callback (datasetForFetch + register/append/push), which
  // runs on SDK connection-pool worker threads against a host DataWriter that
  // has no internal mutex. Self-owned: the plugin no longer relies on the SDK
  // serializing on_done. Lock order is always host_write_mu_ -> fetch_dataset_mu_
  // (the latter taken inside datasetForFetch), never the reverse.
  std::mutex host_write_mu_;
};

}  // namespace mosaico
