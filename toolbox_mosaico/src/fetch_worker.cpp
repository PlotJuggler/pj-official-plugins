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
#include <arrow/record_batch.h>
#include <arrow/table.h>
// clang-format on

#include <fmt/format.h>

#include <chrono>
#include <condition_variable>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "arrow_ingest.hpp"
#include "flight/metadata.hpp"
#include "ontology_routing.h"

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

// Detect whether `schema` already carries a usable timestamp column. Matches
// arrow_ingest::detectTimestampColumn but operates on arrow::Schema for
// convenience inside the worker.
[[nodiscard]] std::string detectTsField(const std::shared_ptr<arrow::Schema>& schema) {
  if (!schema) {
    return {};
  }
  for (const auto& field : schema->fields()) {
    if (field->type()->id() == arrow::Type::TIMESTAMP) {
      return field->name();
    }
  }
  static const std::vector<std::string> kNames = {"timestamp_ns", "recording_timestamp_ns", "timestamp", "time", "ts"};
  for (const auto& cand : kNames) {
    if (schema->GetFieldByName(cand)) {
      return cand;
    }
  }
  return {};
}

// Prepend a synthetic `timestamp_ns` int64 column to a Table for ontology
// payloads (images, point clouds, etc.) that don't carry per-row timestamps
// on the wire. Generates monotonically increasing ns from `start_ns` using
// `interval_ns` between rows.
//
// Image topics in particular ship as one Arrow row per frame with no
// timestamp column — the server uses the Flight ticket metadata for
// frame ordering. We pick a stable monotonic clock anchored at the
// sequence's min_ts_ns when available, otherwise wall-clock.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Table>> prependSyntheticTimestamp(
    std::shared_ptr<arrow::Table> table, std::int64_t start_ns, std::int64_t interval_ns) {
  const int64_t num_rows = table->num_rows();
  arrow::Int64Builder builder;
  ARROW_RETURN_NOT_OK(builder.Reserve(num_rows));
  for (int64_t i = 0; i < num_rows; ++i) {
    ARROW_RETURN_NOT_OK(builder.Append(start_ns + i * interval_ns));
  }
  std::shared_ptr<arrow::Array> ts_array;
  ARROW_RETURN_NOT_OK(builder.Finish(&ts_array));
  auto chunked = std::make_shared<arrow::ChunkedArray>(ts_array);
  return table->AddColumn(0, arrow::field("timestamp_ns", arrow::int64()), chunked);
}

// Ontology routing (isImageOntology / resolveOntologyTag) lives in
// ontology_routing.h so it can be unit-tested without linking Flight/gRPC.
//
// Per-frame image serialization (pushImageRowsToHost) and the per-row Arrow
// column readers (handling BINARY_VIEW / STRING_VIEW) live in arrow_ingest.cpp
// so they can be unit-tested against synthetic *_view tables without Flight.

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
  if (!runtime_host_provider_) {
    return;
  }
  const auto runtime = runtime_host_provider_();
  if (!runtime.valid()) {
    return;
  }
  const PJ_toolbox_runtime_host_t& raw = runtime.raw();
  // Tail-slot gate: an older host's vtable ends before create_parser_ingest —
  // Downloads then run exactly as before, without host progress.
  if (!PJ_HAS_TAIL_SLOT(PJ_toolbox_runtime_host_vtable_t, raw.vtable, create_parser_ingest)) {
    return;
  }
  PJ_data_source_runtime_host_t ingest_raw{};
  PJ_error_t error{};
  if (!raw.vtable->create_parser_ingest(raw.ctx, ds.id, &ingest_raw, &error)) {
    return;  // e.g. parser ingest not configured on this host — degrade silently
  }
  // The context is LIVE on the host from this point: record the id first so
  // finishIngestProgress releases it even when progressStart below fails
  // (release is idempotent and needs no prior progress sequence).
  ingest_ds_id_ = ds.id;
  PJ::DataSourceRuntimeHostView view(ingest_raw);
  // Always indeterminate (total=0): TopicInfo::total_size_bytes is the
  // COMPRESSED full-topic size while progress ticks carry DECODED bytes of the
  // requested slice — no comparable denominator exists, and a wrong one would
  // over/undershoot the host bar wildly. `current` still carries the decoded
  // byte count.
  if (!view.progressStart(sequence_name, /*total_steps=*/0, /*cancellable=*/true).has_value()) {
    return;
  }
  ingest_progress_ = view;
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
      ingest_progress_->progressFinish();
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

  // Per-topic accumulators keyed by topic name. Progress bytes no longer live
  // here — they come from the SDK's accurate decoded-size progress_cb ([I6]).
  struct PerTopic {
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    std::shared_ptr<arrow::Schema> schema;
  };
  auto state = std::make_shared<std::unordered_map<std::string, PerTopic>>();
  auto imported_any = std::make_shared<std::atomic<bool>>(false);
  for (const auto& t : topic_names_std) {
    (*state)[t] = PerTopic{};
  }

  auto on_done = [this, sequence_name, state, imported_any](
                     const std::string& topic_name, arrow::Result<PullResult> result) {
    auto it = state->find(topic_name);
    if (it == state->end()) {
      return;
    }
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
    if (!it->second.schema || it->second.batches.empty()) {
      finish(false, "no data");
      return;
    }
    auto table_result = arrow::Table::FromRecordBatches(it->second.schema, it->second.batches);
    if (!table_result.ok()) {
      finish(false, stringFromArrow(table_result.status()));
      return;
    }
    std::shared_ptr<arrow::Table> table = table_result.ValueOrDie();

    // PJ3-parity: explode struct columns (e.g. nav_msgs/Odometry's pose +
    // twist) into individual primitive columns before handing the stream to
    // the datastore — pj_datastore's arrow_import silently skips non-primitive
    // top-level fields, so without this an Odometry topic shows up as just the
    // timestamp column.
    {
      auto flat_result = flattenStructColumns(std::move(table));
      if (!flat_result.ok()) {
        finish(false, stringFromArrow(flat_result.status()));
        return;
      }
      table = flat_result.ValueOrDie();
    }

    // Route by the authoritative ontology tag (image → ObjectStore; point
    // clouds / poses / scalars → the scalar appendArrowStream path). Also pick
    // up the cached [min,max] ts for synthetic-timestamp anchoring. The tag is
    // cached from getTopicMetadata — which the dialog fetches on topic
    // selection, and the worker processes that slot before this pull (serial
    // queue), so it is populated by now.
    std::int64_t info_min_ts_ns = 0;
    std::int64_t info_max_ts_ns = 0;
    std::string cached_tag;
    if (auto info_it = topic_info_by_name_.find(topic_name); info_it != topic_info_by_name_.end()) {
      info_min_ts_ns = info_it->second.min_ts_ns;
      info_max_ts_ns = info_it->second.max_ts_ns;
      cached_tag = info_it->second.ontology_tag;
    }
    const std::string ontology_tag = resolveOntologyTag(table->schema(), cached_tag);
    const bool is_image = isImageOntology(ontology_tag);
    const bool is_point_cloud = isPointCloudOntology(ontology_tag);
    const bool is_pose = isPoseOntology(ontology_tag);
    const bool is_transform = isTransformOntology(ontology_tag);
    const bool is_occupancy_grid = isOccupancyGridOntology(ontology_tag);
    const bool is_laser_scan = isLaserScanOntology(ontology_tag);
    const bool is_grid_cells = isGridCellsOntology(ontology_tag);
    const bool is_futures_cloud = isFuturesPointCloudOntology(ontology_tag);
    // Canonicalized into a pj_base builtin object (ObjectStore route) instead of
    // scalar columns. These share the single object-push critical section below.
    const bool is_object = isCanonicalObjectOntology(ontology_tag);

    // Determine the timestamp column. Image (and similar media) ontologies
    // ship without per-row timestamps on the wire — the server uses the
    // Flight ticket for frame ordering. If no timestamp column is present,
    // synthesize one anchored at the sequence's min_ts_ns (from the cached
    // TopicInfo) with a ~30 fps cadence so the data still lands in the
    // datastore with monotonic timestamps in the sequence's nominal range.
    std::string ts_field = detectTsField(table->schema());
    std::int64_t synth_anchor_ns = info_min_ts_ns;
    std::int64_t synth_interval_ns = 33'333'333LL;  // ~30 fps default
    if (ts_field.empty()) {
      if (synth_anchor_ns == 0) {
        synth_anchor_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
      }
      const std::int64_t span_ns = info_max_ts_ns - synth_anchor_ns;
      if (table->num_rows() > 1 && span_ns > 0) {
        synth_interval_ns = span_ns / (table->num_rows() - 1);
      }
      // Only augment the table when we'll feed it to the scalar pipeline; the
      // object-push paths (image / point cloud / transform / pose) compute
      // timestamps inline from synth_* directly.
      if (!is_object) {
        auto augmented = prependSyntheticTimestamp(std::move(table), synth_anchor_ns, synth_interval_ns);
        if (!augmented.ok()) {
          finish(false, stringFromArrow(augmented.status()));
          return;
        }
        table = augmented.ValueOrDie();
        ts_field = "timestamp_ns";
      }
    }

    // Canonical-object ontologies route into the ObjectStore: each row becomes
    // one serialized pj_base builtin object (Image / PointCloud / FrameTransforms
    // / PosesInFrame) keyed by timestamp. We deliberately skip pumpStreamToHost
    // for these so the raw payload columns don't ALSO land as opaque scalar
    // series in the datastore. The HOST decodes each blob (the toolbox never
    // decodes/decompresses — see the per-ontology push helpers).
    if (is_object) {
      if (!host_provider_) {
        finish(false, "host not bound");
        return;
      }
      auto host = host_provider_();
      // [C1] Serialize the whole host-write critical section ourselves: this
      // callback runs on SDK connection-pool worker threads and the host
      // DataWriter has no internal mutex. We hold host_write_mu_ around the
      // cached dataset lookup AND every register/push, so the plugin guarantees
      // serialization regardless of whether the SDK serializes on_done.
      //
      // Lock order: host_write_mu_ is acquired ONLY here, and datasetForFetch
      // takes the distinct fetch_dataset_mu_ nested under it (host_write_mu_ ->
      // fetch_dataset_mu_, never the reverse) — so the nesting can't deadlock.
      std::lock_guard<std::mutex> write_lock(host_write_mu_);
      auto ds = datasetForFetch(host, sequence_name);
      if (!ds) {
        finish(false, ds.error());
        return;
      }
      ensureIngestProgress(*ds, sequence_name);
      const ObjectIngestContext ctx{host, *ds, topic_name, ts_field, synth_anchor_ns, synth_interval_ns};
      auto pushed = [&]() -> PJ::Expected<ObjectPushOutcome> {
        if (is_image) {
          return pushImageRowsToHost(ctx, table);
        }
        if (is_point_cloud) {
          return pushPointCloudRowsToHost(ctx, table);
        }
        if (is_pose) {
          return pushPoseRowsToHost(ctx, table);
        }
        if (is_transform) {
          return pushFrameTransformsRowsToHost(ctx, table);
        }
        if (is_occupancy_grid) {
          return pushOccupancyGridRowsToHost(ctx, table);
        }
        if (is_laser_scan) {
          return pushLaserScanRowsToHost(ctx, table);
        }
        if (is_grid_cells) {
          return pushGridCellsRowsToHost(ctx, table);
        }
        if (is_futures_cloud) {
          return pushColumnarPointCloudRowsToHost(ctx, table);
        }
        // Unreachable: is_object (isCanonicalObjectOntology) gates entry, so one
        // branch above always matches. Defensive fallback for a canonical tag
        // added to the routing predicates but not wired to a push helper here.
        return PJ::unexpected(std::string("unhandled canonical ontology '") + ontology_tag + "'");
      }();
      if (!pushed) {
        finish(false, pushed.error());
        return;
      }
      if (pushed->pushed == 0) {
        finish(false, pushed->first_error.empty() ? ("no " + ontology_tag + " rows") : pushed->first_error);
        return;
      }
      // The topic succeeded, but per-row skips are silent in the ObjectPushOutcome
      // — surface them as a non-fatal warning so a partial import (1 good row,
      // N silently dropped) is visible rather than presenting as a clean success.
      std::string warning;
      if (pushed->skipped > 0) {
        warning = ontology_tag + " topic '" + topic_name + "': skipped " + std::to_string(pushed->skipped) + " of " +
                  std::to_string(pushed->pushed + pushed->skipped) + " rows" +
                  (pushed->first_error.empty() ? "" : (" (first: " + pushed->first_error + ")"));
      }
      finish(true, {}, std::move(warning));
      return;
    }

    // Cast Utf8View/BinaryView columns to canonical Utf8/Binary: pj_datastore's
    // import can't parse Arrow view types and nulls the whole batch otherwise.
    {
      auto normalized = normalizeViewColumns(std::move(table));
      if (!normalized.ok()) {
        finish(false, stringFromArrow(normalized.status()));
        return;
      }
      table = normalized.ValueOrDie();
    }
    auto reader = std::make_shared<arrow::TableBatchReader>(*table);
    ArrowArrayStream stream{};
    auto export_status = arrow::ExportRecordBatchReader(reader, &stream);
    if (!export_status.ok()) {
      finish(false, stringFromArrow(export_status));
      return;
    }
    if (!host_provider_) {
      stream.release(&stream);
      finish(false, "host not bound");
      return;
    }
    auto host = host_provider_();
    // [C1] Same self-owned serialization as the image branch: hold
    // host_write_mu_ across datasetForFetch + pumpStreamToHost (the host
    // ensureTopic/appendArrowStream writes). See the lock-order note above.
    std::lock_guard<std::mutex> write_lock(host_write_mu_);
    auto ds = datasetForFetch(host, sequence_name);
    if (!ds) {
      stream.release(&stream);
      finish(false, ds.error());
      return;
    }
    ensureIngestProgress(*ds, sequence_name);
    auto append_status = pumpStreamToHost(host, *ds, topic_name, &stream, ts_field);
    if (!append_status) {
      if (stream.release != nullptr) {
        stream.release(&stream);
      }
      finish(false, append_status.error());
      return;
    }
    finish(true, {});
  };

  auto on_batch = [state](const std::string& topic_name, const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto it = state->find(topic_name);
    if (it == state->end() || !batch) {
      return;
    }
    if (!it->second.schema) {
      it->second.schema = batch->schema();
    }
    it->second.batches.push_back(batch);
  };
  auto on_schema = [state](const std::string& topic_name, const std::shared_ptr<arrow::Schema>& schema) {
    auto it = state->find(topic_name);
    if (it != state->end() && !it->second.schema) {
      it->second.schema = schema;
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
          if (ingest_progress_.has_value()) {
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
    std::mutex poll_wait_mu;
    std::condition_variable_any poll_wait_cv;
    std::jthread host_stop_poller([this, &poll_wait_mu, &poll_wait_cv](std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        if (isStopRequestedByHost()) {
          requestCancelFromHost();
          return;
        }
        std::unique_lock<std::mutex> wait_lock(poll_wait_mu);
        (void)poll_wait_cv.wait_for(wait_lock, stop_token, kHostStopPollInterval, [] { return false; });
      }
    });

    if (pull_topics_override_) {
      pull_topics_override_();
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
