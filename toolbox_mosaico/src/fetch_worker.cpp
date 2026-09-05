// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

// clang-format off
// Arrow's c/abi.h must be included BEFORE any plotjuggler_core header that
// pulls in plugin_data_api.h. Both headers define struct ArrowArrayStream
// under different guard macros (Arrow uses ARROW_C_STREAM_INTERFACE, pj_base
// uses only ARROW_C_DATA_INTERFACE for the whole block). Including Arrow
// first sets both guards so pj_base's #ifndef block becomes a no-op.
// Order is load-bearing — keep clang-format from sorting these.
#include <arrow/c/abi.h>     // must come first
#include <arrow/c/bridge.h>  // transitively re-pulls abi.h, fine

#include "fetch_worker.hpp"

#include <arrow/api.h>
#include <arrow/ipc/writer.h>
#include <arrow/record_batch.h>
#include <arrow/table.h>
// clang-format on

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "arrow_ipc_message.hpp"
#include "flight/metadata.hpp"
#include "ontology_routing.h"
#include "stoppable_thread.hpp"

namespace mosaico {

namespace {

constexpr auto kHostStopPollInterval = std::chrono::milliseconds(50);

#if !defined(PJ_TOOLBOX_HAS_DISCARD_PARSER_INGEST)
// Compatibility layout for SDK 0.20: the paired host appends this slot after
// release_parser_ingest. Keeping it local lets this plugin negotiate the new
// capability while the public SDK package catches up; struct_size preserves
// ABI safety with every older host.
struct ToolboxRuntimeHostDiscardExtension {
  PJ_toolbox_runtime_host_vtable_t base;
  bool (*discard_parser_ingest)(void* ctx, std::uint32_t data_source_id, PJ_error_t* out_error) PJ_NOEXCEPT;
};
#endif

std::string stringFromArrow(const arrow::Status& status) {
  return status.ToString();
}

// Delegated-ingest encoding of scalar topics: one Flight batch = one complete
// Arrow IPC stream, decoded host-side by parser_arrow.
constexpr std::string_view kArrowIpcEncoding = "arrow-ipc";

std::int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

FetchWorker::FetchWorker() = default;
FetchWorker::~FetchWorker() = default;

void FetchWorker::requestCancel() {
  cancel_flag_.store(true, std::memory_order_relaxed);
  cancelActivePulls();
}

void FetchWorker::cancelActivePulls() {
  if (cancel_active_pulls_override_) {
    cancel_active_pulls_override_();
  } else if (client_) {
    client_->cancelActivePulls();
  }
}

PJ::Expected<PJ::sdk::DataSourceHandle> FetchWorker::datasetForFetch(
    const PJ::sdk::ToolboxHostView& host, const std::string& sequence_name) {
  std::lock_guard<std::mutex> lock(fetch_dataset_mu_);
  if (!fetch_dataset_.has_value()) {
    auto ds = host.createDataSource(sequence_name);
    if (!ds) {
      return PJ::unexpected(std::move(ds).error());
    }
    fetch_dataset_ = *ds;
  }
  return *fetch_dataset_;
}

std::uint64_t FetchWorker::accumulatedProgressBytesLocked() const {
  std::uint64_t fetched = 0;
  for (const auto& entry : progress_bytes_by_topic_) {
    const std::int64_t count = entry.second;
    fetched += static_cast<std::uint64_t>(count > 0 ? count : 0);
  }
  return fetched;
}

bool FetchWorker::isStopRequestedByHost() {
  std::lock_guard<std::mutex> plock(progress_mu_);
  return ingest_progress_.has_value() && ingest_progress_->isStopRequested();
}

void FetchWorker::requestCancelFromHost() {
  bool report_stop = false;
  {
    std::lock_guard<std::mutex> plock(progress_mu_);
    if (!host_stop_reported_) {
      host_stop_reported_ = true;
      report_stop = true;
    }
  }
  if (!report_stop) {
    return;
  }
  requestCancel();
  if (hostStopRequested) {
    hostStopRequested();
  }
}

bool FetchWorker::supportsProvisionalIngestDiscard() const {
  if (!runtime_host_provider_) {
    return false;
  }
  const auto runtime = runtime_host_provider_();
  if (!runtime.valid()) {
    return false;
  }
  const auto& raw = runtime.raw();
  if (!PJ_HAS_TAIL_SLOT(PJ_toolbox_runtime_host_vtable_t, raw.vtable, create_parser_ingest)) {
    return false;
  }
#if defined(PJ_TOOLBOX_HAS_DISCARD_PARSER_INGEST)
  return PJ_HAS_TAIL_SLOT(PJ_toolbox_runtime_host_vtable_t, raw.vtable, discard_parser_ingest);
#else
  return raw.vtable->struct_size >= sizeof(ToolboxRuntimeHostDiscardExtension) &&
         reinterpret_cast<const ToolboxRuntimeHostDiscardExtension*>(raw.vtable)->discard_parser_ingest != nullptr;
#endif
}

bool FetchWorker::discardProvisionalIngest(
    const PJ::ToolboxRuntimeHostView& runtime, std::uint32_t data_source_id) const {
#if defined(PJ_TOOLBOX_HAS_DISCARD_PARSER_INGEST)
  return runtime.discardParserIngest(data_source_id).has_value();
#else
  const auto& raw = runtime.raw();
  if (raw.vtable->struct_size < sizeof(ToolboxRuntimeHostDiscardExtension)) {
    return false;
  }
  const auto* extension = reinterpret_cast<const ToolboxRuntimeHostDiscardExtension*>(raw.vtable);
  if (extension->discard_parser_ingest == nullptr) {
    return false;
  }
  PJ_error_t error{};
  return extension->discard_parser_ingest(raw.ctx, data_source_id, &error);
#endif
}

void FetchWorker::ensureIngestProgress(const PJ::sdk::DataSourceHandle& ds, const std::string& sequence_name) {
  std::unique_lock<std::mutex> plock(progress_mu_);
  if (ingest_progress_attempted_) {
    return;
  }
  ingest_progress_attempted_ = true;
  // Every early return below means "no ingest context"; topics report
  // this reason verbatim, so it is the single writer for those paths.
  parser_ingest_error_ = "host offers no parser ingest (arrow-ipc topics need a newer PlotJuggler)";
  if (!runtime_host_provider_) {
    return;
  }
  const auto runtime = runtime_host_provider_();
  if (!runtime.valid()) {
    return;
  }
  const PJ_toolbox_runtime_host_t& raw = runtime.raw();
  // Downloads require the delegated-ingest slot on the host.
  if (!PJ_HAS_TAIL_SLOT(PJ_toolbox_runtime_host_vtable_t, raw.vtable, create_parser_ingest)) {
    return;
  }
  PJ_data_source_runtime_host_t ingest_raw{};
  PJ_error_t error{};
  if (!raw.vtable->create_parser_ingest(raw.ctx, ds.id, &ingest_raw, &error)) {
    // Surface the host's binding failure to the importing topic.
    parser_ingest_error_ = PJ::errorToString(error);
    return;
  }
  // Publish the context used by every topic and record its id for release, even
  // when progressStart below fails.
  parser_ingest_error_.clear();
  ingest_ds_id_ = ds.id;
  ingest_progress_ = PJ::DataSourceRuntimeHostView(ingest_raw);
  // Always indeterminate (total=0): TopicInfo::total_size_bytes is the
  // COMPRESSED full-topic size while progress ticks carry DECODED bytes of the
  // requested slice — no comparable denominator exists, and a wrong one would
  // over/undershoot the host bar wildly. `current` still carries the decoded
  // byte count.
  progress_started_ =
      ingest_progress_->progressStart(sequence_name, /*total_steps=*/0, /*cancellable=*/true).has_value();
  if (!progress_started_) {
    return;  // Only the progress bar is lost; the ingest route stays open.
  }
  const bool host_stop = !ingest_progress_->progressUpdate(accumulatedProgressBytesLocked());
  plock.unlock();
  if (host_stop) {
    requestCancelFromHost();
  }
}

void FetchWorker::finishIngestProgress(bool discard_provisional) {
  std::optional<uint32_t> release_id;
  {
    std::lock_guard<std::mutex> plock(progress_mu_);
    if (ingest_progress_.has_value()) {
      if (progress_started_) {
        ingest_progress_->progressFinish();
      }
      ingest_progress_.reset();
    }
    release_id = ingest_ds_id_;
    ingest_ds_id_.reset();
  }

  // The provisional DataSource can exist even when create_parser_ingest failed
  // (for example, a temporarily unavailable parser service). Rollback is keyed
  // by the DataSource id, so it must not depend on having acquired a progress
  // context successfully.
  std::optional<uint32_t> discard_id;
  if (discard_provisional) {
    std::lock_guard<std::mutex> lock(fetch_dataset_mu_);
    if (fetch_dataset_.has_value()) {
      discard_id = fetch_dataset_->id;
    }
  }

  if ((release_id.has_value() || discard_id.has_value()) && runtime_host_provider_) {
    const auto runtime = runtime_host_provider_();
    if (runtime.valid()) {
      if (discard_id.has_value()) {
        if (discardProvisionalIngest(runtime, *discard_id)) {
          return;
        }
        // A runtime that changed underneath the capability probe still gets a
        // normal release, keeping the progress lifecycle paired.
      }
      if (release_id.has_value()) {
        (void)runtime.releaseParserIngest(*release_id);
      }
    }
  }
}

void FetchWorker::connectAsync(std::string uri, ServerCredentials creds) {
  // creds.allow_insecure is intentionally not consulted here: the plaintext
  // fallback is driven by the caller (onConnectFinished, Step 10.1), which
  // retries with a grpc:// URI. connectAsync always honors the scheme it is given.
  try {
    client_ = std::make_unique<MosaicoClient>(
        uri,
        // PJ3 parity (main_window.cpp:48): 30 s connection timeout for slow links.
        /*timeout_seconds=*/30,
        /*pool_size=*/4, creds.cert_path, creds.api_key);
    auto v = client_->version();
    if (!v.ok()) {
      if (connectFinished) {
        connectFinished({false, {}, v.status().message()});
      }
      client_.reset();
      return;
    }
    if (connectFinished) {
      connectFinished({true, fmt::format("Connected — server {}", v.ValueOrDie().version), {}});
    }
  } catch (const std::exception& e) {
    client_.reset();
    if (connectFinished) {
      connectFinished({false, {}, e.what()});
    }
  } catch (...) {
    client_.reset();
    if (connectFinished) {
      connectFinished({false, {}, "Unknown error"});
    }
  }
}

void FetchWorker::listSequencesAsync() {
  if (!client_) {
    if (sequencesReady) {
      sequencesReady({});
    }
    if (sequenceNamesReady) {
      sequenceNamesReady({});
    }
    return;
  }
  // Progressive discovery (PJ3 parity): on_list_started delivers the initial
  // list so the table can populate immediately, and on_sequence_info reports a
  // completed/total counter as per-sequence detail fills in. Both fire on this
  // worker thread; the dialog routes callbacks to its GUI-thread event queue
  // so the panel updates incrementally during the call.
  auto on_started = [this](const std::vector<SequenceInfo>& seqs) {
    if (sequenceListStarted) {
      sequenceListStarted(seqs);
    }
  };
  auto on_info = [this](const SequenceInfo& seq, int64_t /*completed*/, int64_t /*total*/) {
    if (sequenceInfoReady) {
      sequenceInfoReady(seq);
    }
  };
  auto result = client_->listSequences(on_started, on_info);
  if (!result.ok()) {
    if (sequencesReady) {
      sequencesReady({});
    }
    if (sequenceNamesReady) {
      sequenceNamesReady({});
    }
    return;
  }
  auto seqs = result.ValueOrDie();
  std::vector<std::string> names;
  names.reserve(seqs.size());
  for (const auto& seq : seqs) {
    names.push_back(seq.name);
  }
  if (sequencesReady) {
    sequencesReady(std::move(seqs));
  }
  if (sequenceNamesReady) {
    sequenceNamesReady(std::move(names));
  }
}

void FetchWorker::listTopicsAsync(std::string sequence_name) {
  if (!client_) {
    if (topicsReady) {
      topicsReady(std::move(sequence_name), {});
    }
    return;
  }
  auto result = client_->listTopics(sequence_name);
  if (!result.ok()) {
    // PJ3 parity (fetch_worker.cpp:105-117): NotImplemented means the server
    // doesn't expose topic listing for this sequence — treat as an empty list
    // and let the user pick topics manually. Any other status is a real error
    // (auth, transport, server fault) and MUST be surfaced rather than silently
    // collapsed to "no topics".
    if (result.status().IsNotImplemented()) {
      if (topicsReady) {
        topicsReady(std::move(sequence_name), {});
      }
    } else {
      if (topicsReady) {
        // Clear the "loading…" header / stale rows for this sequence.
        topicsReady(sequence_name, {});
      }
      if (errorOccurred) {
        errorOccurred(fmt::format("listTopics {}: {}", sequence_name, result.status().ToString()));
      }
    }
    return;
  }
  // Cache per-topic info keyed by name so subsequent pulls can route image
  // ontologies through the synthetic-timestamp path without an extra
  // server round-trip.
  topic_info_by_name_.clear();
  topic_info_by_name_.reserve(result.ValueOrDie().size());
  std::vector<std::string> names;
  std::vector<TopicInfo> infos = result.ValueOrDie();
  names.reserve(infos.size());
  for (const auto& t : infos) {
    names.push_back(t.topic_name);
    topic_info_by_name_.emplace(t.topic_name, t);
  }
  if (topicsReady) {
    topicsReady(sequence_name, std::move(names));
  }
  if (topicInfosReady) {
    topicInfosReady(std::move(sequence_name), std::move(infos));
  }
}

void FetchWorker::fetchTopicMetadataAsync(TopicRef topic) {
  if (!client_) {
    return;
  }
  auto result = client_->getTopicMetadata(topic.sequence_name, topic.topic_name);
  if (!result.ok()) {
    return;
  }
  TopicInfo info = result.ValueOrDie();
  // Merge the size/created/locked fields cached from listTopics — getTopicMetadata
  // only fills schema/ontology/user_metadata/timestamps, not total_size_bytes
  // or chunks_number.
  if (auto it = topic_info_by_name_.find(topic.topic_name); it != topic_info_by_name_.end()) {
    if (info.total_size_bytes == 0) {
      info.total_size_bytes = it->second.total_size_bytes;
    }
    if (info.chunks_number == 0) {
      info.chunks_number = it->second.chunks_number;
    }
    if (info.created_at_ns == 0) {
      info.created_at_ns = it->second.created_at_ns;
    }
    if (!info.completed_at_ns.has_value()) {
      info.completed_at_ns = it->second.completed_at_ns;
    }
    if (!info.locked) {
      info.locked = it->second.locked;
    }
    if (info.resource_locator.empty()) {
      info.resource_locator = it->second.resource_locator;
    }
    // Refresh the cache so a later pull reuses the fuller record.
    it->second = info;
  }
  if (topicMetadataReady) {
    topicMetadataReady(std::move(topic), std::move(info));
  }
}

void FetchWorker::pullTopicsAsync(
    std::string sequence_name, std::vector<std::string> topic_names, std::int64_t start_ns, std::int64_t end_ns) {
  if (!client_ && !pull_topics_override_) {
    for (const auto& t : topic_names) {
      if (pullFinished) {
        pullFinished({{sequence_name, t}, false, "not connected", {}});
      }
    }
    if (allFetchesComplete) {
      allFetchesComplete(std::move(sequence_name));
    }
    return;
  }
  if (topic_names.empty()) {
    if (allFetchesComplete) {
      allFetchesComplete(std::move(sequence_name));
    }
    return;
  }
  // Start of a multi-topic Download: discard any DataSourceHandle cached from a
  // previous fetch. datasetForFetch creates exactly one dataset (eagerly only
  // when rollback is available), and every topic callback reuses it so the
  // catalog shows ONE group, not one per topic.
  {
    std::lock_guard<std::mutex> lock(fetch_dataset_mu_);
    fetch_dataset_ = std::nullopt;
  }
  // Fresh host-progress bracket for this batch.
  {
    std::lock_guard<std::mutex> plock(progress_mu_);
    ingest_progress_.reset();
    ingest_progress_attempted_ = false;
    progress_started_ = false;
    parser_ingest_error_.clear();
    host_stop_reported_ = false;
    ingest_ds_id_.reset();
    progress_bytes_by_topic_.clear();
  }

  // Use the SDK's parallel pullTopics. Per-topic completion (on_done) fires on
  // SDK connection-pool worker threads; the host-write critical section there
  // is serialized by our own host_write_mu_ ([C1]) rather than relying on the
  // SDK's internal mutex. pullFinished etc. route to the GUI thread through
  // the dialog's event queue.
  std::vector<std::string> topic_names_std = std::move(topic_names);
  TimeRange range;
  range.start_ns = start_ns;
  range.end_ns = end_ns;

  // Per-topic state keyed by topic name. Fully populated before the pull starts
  // and never rehashed, so concurrent access to DIFFERENT keys is safe; one
  // topic's callbacks arrive in order on one pool thread.
  struct PerTopic {
    std::shared_ptr<arrow::Schema> schema;
    bool routed = false;        // route() has run; its results below are final
    bool object_route = false;  // canonical objects use one message per row
    std::string ontology_tag;
    std::string ts_field;       // flattened leaf path; may be unnamed
    std::vector<int> ts_route;  // child-index route; empty = no timestamp column
    std::int64_t info_max_ts_ns = 0;
    // Unstamped topics wait for the total row count before fitting cadence.
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    // Binding inputs are framed before taking the host-write locks.
    std::shared_ptr<arrow::Buffer> ipc_schema_bytes;
    std::string parser_config;
    std::optional<PJ::ParserBindingHandle> binding;
    std::int64_t synth_anchor_ns = 0;
    std::int64_t synth_interval_ns = kSyntheticIntervalNs;
    std::int64_t rows_pushed = 0;
    std::int64_t last_ipc_bytes = 0;  // sizes the next batch's output buffer
    std::string error;                // first bind/push failure; fails the topic at on_done
  };
  auto state = std::make_shared<std::unordered_map<std::string, PerTopic>>();
  auto imported_any = std::make_shared<std::atomic<bool>>(false);
  for (const auto& t : topic_names_std) {
    (*state)[t] = PerTopic{};
  }

  // Resolve ontology and timestamp route once from the source metadata/schema.
  auto route = [this](PerTopic& topic, const std::string& topic_name) {
    if (topic.routed || !topic.schema) {
      return;
    }
    topic.routed = true;
    std::string cached_tag;
    std::int64_t info_min_ts_ns = 0;
    if (auto info_it = topic_info_by_name_.find(topic_name); info_it != topic_info_by_name_.end()) {
      cached_tag = info_it->second.ontology_tag;
      info_min_ts_ns = info_it->second.min_ts_ns;
      topic.info_max_ts_ns = info_it->second.max_ts_ns;
    }
    topic.ontology_tag = resolveOntologyTag(topic.schema, cached_tag);
    topic.object_route = isCanonicalObjectOntology(topic.ontology_tag);
    topic.synth_anchor_ns = info_min_ts_ns != 0 ? info_min_ts_ns : nowNs();
    auto leaf =
        detectTimestampLeaf(*topic.schema, topic.object_route ? EmptyNameRule::kFlatten : EmptyNameRule::kIndex);
    topic.ts_field = std::move(leaf.path);
    topic.ts_route = std::move(leaf.route);
  };

  // Cadence for a timestamp-less topic: spread @p total_rows over the topic's
  // [min_ts_ns, max_ts_ns] range, else keep the ~30 fps default.
  auto fitSyntheticInterval = [](PerTopic& topic, std::int64_t total_rows) {
    if (topic.ts_route.empty()) {
      topic.synth_interval_ns =
          PJ::fitSyntheticInterval(topic.synth_anchor_ns, topic.info_max_ts_ns, total_rows, kSyntheticIntervalNs);
    }
  };

  // Hand the batch to the host as an arrow-ipc message. Runs on
  // the pool thread that received the batch — or, for a timestamp-less topic,
  // on the one that completed it; the host-write section is
  // serialized by host_write_mu_ ([C1]) and the ingest fat pointer by
  // progress_mu_ (same order as ensureIngestProgress), so the context is only
  // ever driven by one caller at a time as its contract requires.
  auto push_message = [this, sequence_name, imported_any](
                          PerTopic& topic, const std::string& topic_name, const arrow::RecordBatch& batch) {
    if (!topic.error.empty() || batch.num_rows() == 0) {
      return;
    }
    if (batch.num_rows() > std::numeric_limits<std::int64_t>::max() - topic.rows_pushed) {
      topic.error = "topic row count overflow";
      return;
    }
    std::shared_ptr<arrow::RecordBatch> timed_batch;
    if (topic.ts_route.empty()) {
      auto timed = addSyntheticTimestamps(batch, topic.synth_anchor_ns, topic.synth_interval_ns, topic.rows_pushed);
      if (!timed.ok()) {
        topic.error = stringFromArrow(timed.status());
        return;
      }
      timed_batch = *timed;
    }
    const arrow::RecordBatch& framed = timed_batch ? *timed_batch : batch;
    if (!topic.ipc_schema_bytes) {
      auto schema_bytes = arrow::ipc::SerializeSchema(*framed.schema());
      if (!schema_bytes.ok()) {
        topic.error = stringFromArrow(schema_bytes.status());
        return;
      }
      topic.ipc_schema_bytes = *schema_bytes;
      topic.parser_config = parserConfigJson(timed_batch ? framed.column_name(0) : topic.ts_field, 0);
    }
    auto bytes = serializeIpcStream(framed, topic.last_ipc_bytes);
    if (!bytes.ok()) {
      topic.error = stringFromArrow(bytes.status());
      return;
    }
    topic.last_ipc_bytes = (*bytes)->size();
    // The envelope uses the first row's timestamp, including a synthesized axis.
    auto host_ts = firstRowTimestampNs(framed, timed_batch ? std::vector<int>{0} : topic.ts_route);
    if (!host_ts) {
      host_ts = PJ::syntheticInstant(topic.synth_anchor_ns, topic.synth_interval_ns, topic.rows_pushed);
      if (!host_ts) {
        topic.error = fmt::format(
            "synthetic timestamp overflows int64 at row {} (anchor {}, interval {})", topic.rows_pushed,
            topic.synth_anchor_ns, topic.synth_interval_ns);
        return;
      }
    }
    const std::int64_t host_ts_ns = *host_ts;
    if (!host_provider_) {
      topic.error = "host not bound";
      return;
    }
    auto host = host_provider_();
    std::lock_guard<std::mutex> write_lock(host_write_mu_);
    auto ds = datasetForFetch(host, sequence_name);
    if (!ds) {
      topic.error = ds.error();
      return;
    }
    ensureIngestProgress(*ds, sequence_name);
    // ponytail: progress_mu_ is held across pushMessage, so a slow host push
    // stalls the 50 ms stop poller and the progress ticks, and host_write_mu_
    // serializes concurrent scalar topics batch by batch. Upgrade path if a
    // profile shows it: hand batches to a per-topic queue drained by one thread.
    std::lock_guard<std::mutex> plock(progress_mu_);
    if (!ingest_progress_.has_value()) {
      topic.error = parser_ingest_error_;  // ensureIngestProgress always leaves a reason
      return;
    }
    if (!topic.binding.has_value()) {
      auto binding = ingest_progress_->ensureParserBinding(
          PJ::ParserBindingRequest{
              .topic_name = topic_name,
              .parser_encoding = kArrowIpcEncoding,
              .type_name = topic.ontology_tag,
              .schema = PJ::Span<const uint8_t>(
                  topic.ipc_schema_bytes->data(), static_cast<std::size_t>(topic.ipc_schema_bytes->size())),
              .parser_config_json = topic.parser_config,
          });
      if (!binding) {
        topic.error = binding.error();
        return;
      }
      topic.binding = *binding;
    }
    // Zero-copy: the IPC buffer is its own anchor, the host keeps it alive.
    auto pushed = ingest_progress_->pushMessage(*topic.binding, host_ts_ns, [payload = std::move(*bytes)]() {
      return PJ::sdk::PayloadView(
          PJ::Span<const uint8_t>(payload->data(), static_cast<std::size_t>(payload->size())),
          PJ::sdk::BufferAnchor(payload));
    });
    if (!pushed) {
      topic.error = pushed.error();
      return;
    }
    topic.rows_pushed += batch.num_rows();
    imported_any->store(true, std::memory_order_relaxed);
  };

  auto push_batch = [push_message](PerTopic& topic, const std::string& name, const arrow::RecordBatch& batch) {
    if (topic.object_route) {
      // The host's functional object contract is one object per raw message.
      for (std::int64_t row = 0; row < batch.num_rows() && topic.error.empty(); ++row) {
        push_message(topic, name, *batch.Slice(row, 1));
      }
    } else {
      push_message(topic, name, batch);
    }
  };

  auto on_done = [this, sequence_name, state, imported_any, push_batch, fitSyntheticInterval](
                     const std::string& topic_name, arrow::Result<PullResult> result) {
    auto it = state->find(topic_name);
    if (it == state->end()) {
      return;
    }
    PerTopic& topic = it->second;
    auto finish = [this, &sequence_name, &topic_name, imported_any](
                      bool ok, std::string error, std::string warning = {}) {
      if (ok) {
        imported_any->store(true, std::memory_order_relaxed);
      }
      if (pullFinished) {
        pullFinished({{sequence_name, topic_name}, ok, std::move(error), std::move(warning)});
      }
    };
    if (!result.ok()) {
      (void)client_->reportTopicNotification(sequence_name, topic_name, "fetch_error", result.status().message());
      finish(false, stringFromArrow(result.status()));
      return;
    }
    if (!topic.schema) {
      finish(false, "no data");
      return;
    }
    // A stamped topic streamed batch by batch through push_batch and
    // leaves nothing buffered. A timestamp-less one waited here: its cadence
    // is fitted to the topic's [min,max] range over the WHOLE row count, so
    // the timestamp column and every host timestamp need the total first.
    // Batches that never reached this point (cancel, transport failure) are
    // dropped, exactly as when the whole topic was written at on_done. So a
    // cancelled Download is asymmetric BY DESIGN: a stamped topic keeps the
    // batches already pushed, a timestamp-less one keeps nothing, because a
    // partial row count would fit a cadence the rest of the topic contradicts.
    if (topic.error.empty() && !topic.batches.empty()) {
      std::int64_t total_rows = 0;
      for (const auto& buffered : topic.batches) {
        if (buffered->num_rows() > std::numeric_limits<std::int64_t>::max() - total_rows) {
          finish(false, "topic row count overflow");
          return;
        }
        total_rows += buffered->num_rows();
      }
      fitSyntheticInterval(topic, total_rows);
      for (const auto& buffered : topic.batches) {
        push_batch(topic, topic_name, *buffered);
      }
    }
    topic.batches.clear();
    if (!topic.error.empty()) {
      finish(false, topic.error);
    } else if (topic.rows_pushed == 0) {
      finish(false, "no data");
    } else {
      finish(true, {});
    }
  };

  auto on_batch = [state, route, push_batch](
                      const std::string& topic_name, const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto it = state->find(topic_name);
    if (it == state->end() || !batch) {
      return;
    }
    PerTopic& topic = it->second;
    if (!topic.schema) {
      // One schema per topic, cached from whichever of on_schema/on_batch lands
      // first: Flight gives a stream ONE schema, so there is deliberately no
      // evolution machinery here — every later batch is framed against this one.
      topic.schema = batch->schema();
    }
    route(topic, topic_name);
    if (!topic.error.empty()) {
      return;  // route() refused the topic; on_done reports it, buffering would only waste memory
    }
    // Stamped topics stream immediately; unstamped topics need the total row count.
    if (topic.ts_route.empty()) {
      topic.batches.push_back(batch);
    } else {
      push_batch(topic, topic_name, *batch);
    }
  };
  auto on_schema = [state, route](const std::string& topic_name, const std::shared_ptr<arrow::Schema>& schema) {
    auto it = state->find(topic_name);
    if (it != state->end() && !it->second.schema) {
      it->second.schema = schema;
      route(it->second, topic_name);
    }
  };
  // [I6] Real progress bytes. The SDK already computes the TRUE decoded batch
  // size (decodedRecordBatchBytes) and delivers the running cumulative `bytes`
  // here per topic — far more accurate than the old rows*cols*8 estimate, which
  // was meaningless for variable-length / image columns. Forward that value
  // straight into the existing pullProgress callback (shape unchanged). The SDK
  // does NOT serialize this callback (topics report concurrently), but the
  // dialog's event enqueue is thread-safe, so this is fine.
  auto on_progress =
      [this](const std::string& topic_name, std::int64_t /*rows*/, std::int64_t bytes, std::int64_t /*total_bytes*/) {
        if (pullProgress) {
          pullProgress(topic_name, bytes);
        }
        // Host progress tick. progress_mu_ serializes the fat-pointer call
        // (single-caller surface; these events arrive on concurrent pool
        // threads) and guards the byte ledger. The host throttles internally
        // (~50 ms), so most ticks return immediately. A false return is the
        // host's stop request (title-bar Stop).
        bool host_stop = false;
        {
          std::lock_guard<std::mutex> plock(progress_mu_);
          progress_bytes_by_topic_[topic_name] = bytes;
          if (ingest_progress_.has_value() && progress_started_) {
            const std::uint64_t fetched = accumulatedProgressBytesLocked();
            if (!ingest_progress_->progressUpdate(fetched) && !host_stop_reported_) {
              host_stop = true;
            }
          }
        }
        if (host_stop) {
          requestCancelFromHost();
        }
      };

  // Cancel is owned/reset by the dialog at fetch start (buttonFetch handler, on
  // the GUI thread). We deliberately do NOT resetCancel() here: this method runs
  // on the worker thread after the queued fetch post, so a reset here would
  // race a Cancel clicked in that window and clear the just-set flag. [B1]
  // Always emit allFetchesComplete, even if pullTopics throws unexpectedly:
  // the dialog clears fetch_active (and re-enables Close) only on that signal,
  // so a swallowed completion would strand the panel with Close disabled.
  try {
    // A real DataSource may be created before the first byte only when the
    // paired host can roll it back. Older hosts have no remove operation, so
    // they retain the lazy-on-first-importable-topic path in on_done and cannot
    // accumulate empty datasets after transport failures, no-data responses,
    // or cancellation.
    if (host_provider_ && supportsProvisionalIngestDiscard()) {
      const auto host = host_provider_();
      std::lock_guard<std::mutex> write_lock(host_write_mu_);
      auto ds = datasetForFetch(host, sequence_name);
      if (!ds) {
        // One failure per requested topic: the dialog's ledger is keyed by topic
        // name, so a single synthetic result would strand the tally at 1/N and
        // show a blank-named row.
        for (const auto& t : topic_names_std) {
          if (pullFinished) {
            pullFinished({{sequence_name, t}, false, fmt::format("create dataset failed: {}", ds.error()), {}});
          }
        }
        finishIngestProgress(/*discard_provisional=*/false);
        if (allFetchesComplete) {
          allFetchesComplete(std::move(sequence_name));
        }
        return;
      }
      ensureIngestProgress(*ds, sequence_name);
    }

    // A silent transport still has a live control plane: the ingest context's
    // Stop flag is thread-safe and does not depend on progress callbacks.
    // StoppableThread rather than std::jthread + std::stop_token: neither exists
    // in Apple's libc++, which is why 1.1.0 shipped with no macOS artifacts. Same
    // two properties as before -- the destructor stops and joins on every exit
    // path, and the wait returns the moment stop is requested instead of sitting
    // out the remaining poll interval.
    StoppableThread host_stop_poller([this](StoppableThread& poller) {
      while (!poller.stopRequested()) {
        if (isStopRequestedByHost()) {
          requestCancelFromHost();
          return;
        }
        poller.waitForStop(kHostStopPollInterval);
      }
    });

    if (pull_topics_override_) {
      pull_topics_override_(on_batch, on_done);
    } else {
      (void)client_->pullTopics(
          sequence_name, topic_names_std, range, on_done, on_progress, &cancel_flag_, on_batch, on_schema,
          /*retain_batches=*/false);
    }
  } catch (const std::exception& e) {
    if (pullFinished) {
      pullFinished({{sequence_name, {}}, false, fmt::format("pull failed: {}", e.what()), {}});
    }
  } catch (...) {
    if (pullFinished) {
      pullFinished({{sequence_name, {}}, false, "pull failed: unknown error", {}});
    }
  }
  // Bracket the host progress/stop channel on EVERY exit (success, cancel,
  // throw) and BEFORE allFetchesComplete: a zero-success provisional source is
  // rolled back; otherwise release lands before the dialog's terminal
  // notifyDataChanged so the host can focus/reconcile the finished dataset.
  finishIngestProgress(/*discard_provisional=*/!imported_any->load(std::memory_order_relaxed));
  if (allFetchesComplete) {
    allFetchesComplete(std::move(sequence_name));
  }
}

}  // namespace mosaico
