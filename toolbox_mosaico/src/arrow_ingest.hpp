// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <arrow/c/abi.h>

#include <cstdint>
#include <memory>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <string>
#include <string_view>

// Forward-declared so the header doesn't pull in <arrow/api.h>.
namespace arrow {
class Table;
template <typename T>
class Result;
}  // namespace arrow

namespace mosaico {

/// Auto-detect the timestamp column name in an Arrow Schema:
///   1. Any field whose type is TIMESTAMP.
///   2. Otherwise, the first name in {timestamp_ns, recording_timestamp_ns,
///      timestamp, time, ts}.
/// Returns the detected name or "" if none.
[[nodiscard]] std::string detectTimestampColumn(const ArrowSchema* schema);

/// Walk every column of @p table and flatten any STRUCT-typed columns into
/// individual primitive columns named `<parent>/<child>`. Mirrors PJ3's
/// `flattenArray` (toolbox_mosaico.cpp:341) — without it, ROS-shaped topics
/// like `nav_msgs/Odometry` (whose `pose` and `twist` are struct columns)
/// reach the host as opaque struct entries that PJ4's arrow_import silently
/// drops, so the dataset tree only shows the timestamp.
///
/// Non-struct columns pass through unchanged. List/map/union columns also
/// pass through — PJ3 only handles primitive + struct; matching that.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Table>> flattenStructColumns(std::shared_ptr<arrow::Table> table);

/// Cast Arrow "view" string/binary columns (Utf8View / BinaryView) to canonical
/// Utf8 / Binary. pj_datastore's nanoarrow import only maps STRING / LARGE_STRING
/// and a view-typed column corrupts the whole record-batch import (every column
/// reads null), so the scalar appendArrowStream pipeline normalizes them first.
/// Columns without a view type pass through unchanged.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Table>> normalizeViewColumns(std::shared_ptr<arrow::Table> table);

/// Pump an ArrowArrayStream into the toolbox host as a single topic under a
/// previously created data source.
///
/// `source` is the per-Download DataSourceHandle (one per sequence, shared by
/// every topic — see FetchWorker::datasetForFetch). `topic_name` is the BARE
/// topic name; it is NOT prefixed with the sequence, because the data source
/// already carries the sequence name. `timestamp_col` is the field to use as
/// the X axis; the caller should run detectTimestampColumn against the
/// stream's schema first.
///
/// Takes ownership of `stream` on success — the host's appendArrowStream
/// implementation calls stream->release. On failure the caller still owns
/// the stream and must release it.
[[nodiscard]] PJ::Status pumpStreamToHost(
    const PJ::sdk::ToolboxHostView& host, PJ::sdk::DataSourceHandle source, std::string_view topic_name,
    ArrowArrayStream* stream, std::string_view timestamp_col);

/// Outcome of pushImageRowsToHost. A non-fatal per-row skip (missing data,
/// missing geometry, …) increments `skipped` and records the first reason in
/// `first_error` without aborting the topic — `pushed` is the count actually
/// serialized and handed to the host.
struct ImagePushOutcome {
  std::int64_t pushed = 0;
  std::int64_t skipped = 0;
  std::string first_error;
};

/// Serialize every row of @p table as a canonical PJ.Image blob (pj_base's
/// PJ::serializeImage) and push it into the host's ObjectStore under @p source,
/// keyed by the BARE @p topic_name.
///
/// The object topic is registered ONCE with the canonical metadata JSON
///   {"builtin_object_type":"kImage","image_codec":"pj_image_v1"}
/// — geometry is per-frame inside each blob, not topic-level.
///
/// Per row it reads width/height/stride (int32), encoding+format (string;
/// STRING/LARGE_STRING/STRING_VIEW), is_bigendian (bool; null/absent -> host
/// native endianness, per the canonical Image model),
/// data (binary; BINARY/LARGE_BINARY/FIXED_SIZE_BINARY/BINARY_VIEW), and the
/// timestamp from @p ts_field if present (otherwise synth_anchor + row*interval).
/// `encoding` falls back to `format` when empty so pure-compressed topics
/// (only `format`) still carry an encoding. A row missing required columns is
/// skipped (see ImagePushOutcome) rather than aborting the topic.
///
/// Returns an error only on a fatal failure (host unbound, missing `data`
/// column entirely, or a host register/push rejection).
[[nodiscard]] PJ::Expected<ImagePushOutcome> pushImageRowsToHost(
    const PJ::sdk::ToolboxHostView& host, PJ::sdk::DataSourceHandle source, const std::string& topic_name,
    const std::shared_ptr<arrow::Table>& table, const std::string& ts_field, std::int64_t synth_anchor_ns,
    std::int64_t synth_interval_ns);

}  // namespace mosaico
