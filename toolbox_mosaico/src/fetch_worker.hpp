// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <unordered_map>

#include "flight/mosaico_client.hpp"
#include "flight/types.hpp"

namespace mosaico {

/// Thin background adapter for MosaicoClient running on a worker QThread.
/// All public slots execute on the worker thread; signals are routed back
/// to the GUI thread via Qt's queued-connection machinery.
class FetchWorker : public QObject {
  Q_OBJECT
 public:
  explicit FetchWorker(QObject* parent = nullptr);
  ~FetchWorker() override;

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

 public Q_SLOTS:
  /// Connect (or reconnect) to the given URI. Posts connectFinished on completion.
  void connectAsync(QString uri, QString cert_path, QString api_key, bool allow_insecure);

  /// List all sequences from the connected server.
  void listSequencesAsync();

  /// List topics for a given sequence (partial metadata).
  void listTopicsAsync(QString sequence_name);

  /// Fetch full per-topic metadata (Arrow schema, ontology tag, user
  /// metadata) on demand for the Info panel. Posts topicMetadataReady.
  void fetchTopicMetadataAsync(QString sequence_name, QString topic_name);

  /// Pull several topics of the same sequence in parallel via the SDK's
  /// connection pool. Each topic emits its own pullProgress / pullFinished
  /// signal; the per-topic host-write section is serialized by host_write_mu_
  /// so the plugin's bookkeeping is safe regardless of SDK-internal locking.
  /// After all topics complete, allFetchesComplete fires once.
  void pullTopicsAsync(QString sequence_name, QStringList topic_names, qint64 start_ns, qint64 end_ns);

 Q_SIGNALS:
  void connectFinished(bool ok, QString status, QString error);
  /// Full SequenceInfo entries, including user_metadata (used by the Lua
  /// metadata filter). The QStringList overload is a separate signal kept
  /// for code paths that only want names.
  void sequencesReady(std::vector<SequenceInfo> sequences);
  void sequenceNamesReady(QStringList names);
  // Progressive discovery (PJ3 parity): the initial list, so the table can
  // populate before per-sequence detail finishes streaming in...
  void sequenceListStarted(std::vector<SequenceInfo> sequences);
  // ...and one signal per sequence as the server fills in its detail
  // (min/max timestamp, size, metadata), so Date/Size columns populate
  // incrementally rather than snapping in all at once.
  void sequenceInfoReady(SequenceInfo sequence);
  void topicsReady(QString sequence_name, QStringList topic_names);
  /// Full TopicInfo list from listTopics (name + size + timestamp range +
  /// created/locked/chunks). Schema/ontology/user_metadata are NOT populated
  /// here — those arrive via topicMetadataReady after fetchTopicMetadataAsync.
  void topicInfosReady(QString sequence_name, std::vector<TopicInfo> topics);
  /// Full per-topic metadata (incl. Arrow schema) for the Info panel.
  void topicMetadataReady(QString sequence_name, QString topic_name, TopicInfo info);
  void pullProgress(QString topic_name, qint64 bytes);
  void pullFinished(QString sequence_name, QString topic_name, bool ok, QString error);
  void allFetchesComplete(QString sequence_name);

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
