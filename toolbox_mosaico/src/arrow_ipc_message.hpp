// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

// Framing helpers for the `arrow-ipc` delegated-ingest encoding: one Flight
// record batch becomes one self-contained Arrow IPC stream (schema + batch +
// EOS), which is exactly what `parser_arrow` decodes. Uses the SDK timestamp policy.

#include <arrow/result.h>
#include <arrow/type_fwd.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <pj_base/time_math.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace mosaico {

/// Cadence of the synthetic time axis for rows without a timestamp column
/// (~30 fps). Passed to parser_arrow as `synthetic_interval_ns`; the message
/// host timestamp advances by it per row so the parser continues the axis
/// seamlessly across batches.
inline constexpr std::int64_t kSyntheticIntervalNs = PJ::kDefaultSyntheticIntervalNs;

/// Where one framed field comes from and how to build it.
struct FieldProjection {
  /// Index of the source field in its parent (a batch column, or a struct child).
  int source_index = 0;
  /// The framed type.
  std::shared_ptr<arrow::DataType> type;
  /// Populated ONLY for a struct that lost children: the survivors, in output
  /// order. Empty means "take the source as-is and cast it" — the common path,
  /// and what keeps an untouched column zero-copy.
  std::vector<FieldProjection> children;
};

/// What a topic's schema becomes on the wire, plus what it cost to get there.
struct IpcSafeSchema {
  /// The schema to frame. The SAME pointer as the input when nothing changed,
  /// so callers skip the cast pass (and stay zero-copy) with a pointer compare.
  std::shared_ptr<arrow::Schema> schema;
  /// One entry per framed column, in output order.
  std::vector<FieldProjection> columns;
  /// One `field '<path>': type <T> <reason>` message per field that had to be
  /// dropped. Empty on the common path; a topic-level warning otherwise.
  std::vector<std::string> dropped;
};

/// Rewrite @p schema into one parser_arrow can decode. An ALLOWLIST of what
/// nanoarrow_ipc 0.7 accepts: view types collapse to their materialized form,
/// dictionary and extension fields to the type underneath, and containers
/// recurse.
///
/// A field that cannot be reduced — a union whose child needs a rewrite, a
/// run-end encoding, any type outside the allowlist — is DROPPED and named in
/// `dropped` rather than failing the topic: PJ3 flattened and let the datastore
/// skip what it could not plot, and one exotic field should not cost the user
/// every sibling. The drop reaches INSIDE structs — a struct keeps the children
/// that frame and loses only those that do not, going entirely when none
/// survive — which is why the projection is a tree, not a column list. A struct
/// below a list/map/union stays all-or-nothing: no kernel rebuilds those parents
/// around a projected child. Fails only when no top-level column survives.
///
/// This side only asserts the allowlist; that nanoarrow really decodes the
/// result is parser_arrow's test suite to prove.
[[nodiscard]] arrow::Result<IpcSafeSchema> ipcSafeSchema(const std::shared_ptr<arrow::Schema>& schema);

/// Rebuild @p batch through `safe.columns`: keep, cast, or — for a struct that
/// lost children — reassemble around the survivors, recursively.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::RecordBatch>> castToSchema(
    const arrow::RecordBatch& batch, const IpcSafeSchema& safe);

/// How to name a field whose name is empty — the ONE point where this
/// detector's two consumers disagree, so the caller states which it is.
enum class EmptyNameRule {
  kIndex,    ///< `_<child index>`: parser_arrow's childOutputName (scalar route)
  kFlatten,  ///< nothing, leaving a trailing `parent/`: Arrow's Table::Flatten (object route)
};

/// A topic's timestamp leaf: struct children expand depth-first into
/// `parent/child` and every literal '.' in a component becomes '/'
/// (`wheel.speed` -> `wheel/speed`; `header.stamp` inside struct `msg` ->
/// `msg/header/stamp`). An empty component follows @p empty_name_rule; under
/// kIndex that also keeps an unnamed top-level timestamp (`_0`) distinct from
/// the empty string that means "this topic has no timestamp".
struct TimestampLeaf {
  std::string path;        ///< empty when the schema has no timestamp leaf
  std::vector<int> route;  ///< top-level column index, then one struct-child index per level
};

/// Pick the timestamp leaf using the SDK's canonical names and storage eligibility.
/// Use the schema and empty-name convention the consumer will see: IPC-safe with
/// kIndex for scalars, raw with kFlatten for the object route's Table::Flatten.
[[nodiscard]] TimestampLeaf detectTimestampLeaf(
    const arrow::Schema& schema, EmptyNameRule empty_name_rule, PJ::TimeUnit unit = PJ::TimeUnit::kNanoseconds);

/// Name the axis @p raw carried that framing then LOST — the same two rules as
/// detectTimestampLeaf minus the type gate, restricted to leaves that are no
/// longer leaves of @p framed. Empty when the producer sent no axis-shaped leaf,
/// or when the leaf is still there: a `time` column of a type parser_arrow will
/// not accept as an axis was never lost, just unused, and its topic imports on
/// the synthetic cadence. Only the fatal "cannot be framed" check wants this;
/// everything else asks detectTimestampLeaf what the parser WILL use.
[[nodiscard]] std::string axisLostToFraming(
    const arrow::Schema& raw, const arrow::Schema& framed, EmptyNameRule empty_name_rule);

/// Row 0 of the leaf at @p route as nanoseconds: FLOAT/DOUBLE are seconds,
/// TIMESTAMP is rescaled by its native unit, integers use @p unit (default ns).
/// Empty for an empty route (how a timestamp-less topic is marked), an empty
/// batch, a null value or null ancestor, or a value that does not convert
/// safely. Half-float is deliberately NOT accepted — parser_arrow refuses it as
/// an axis, so stamping from it here would disagree with the parser.
///
/// Only the message's host timestamp — the parser reads per-row time from the
/// column. Pass the batch the route was computed against: for a scalar topic
/// that is the CAST batch, whose columns the drop projection may have renumbered.
[[nodiscard]] std::optional<std::int64_t> firstRowTimestampNs(
    const arrow::RecordBatch& batch, const std::vector<int>& route, PJ::TimeUnit unit = PJ::TimeUnit::kNanoseconds);

/// One record batch as a complete IPC stream (schema message, the batch, end
/// of stream). Uncompressed: the bytes cross a process-internal seam.
/// @p capacity_hint sizes the output buffer up front (the previous batch's
/// size is a good guess); 0 falls back to a small default.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Buffer>> serializeIpcStream(
    const arrow::RecordBatch& batch, std::int64_t capacity_hint = 0);

/// `parser_arrow` configuration for one topic: `{"timestamp_column": <leaf
/// path>, "timestamp_unit": <unit>, "synthetic_interval_ns": <interval>, "flatten_structs": true}`.
///
/// The interval is per-topic, not a constant: a timestamp-less topic spreads its
/// rows over the topic's [min,max] range, so the caller passes the fitted value.
/// `flatten_structs` is pinned rather than left to the parser's default because
/// a nested `timestamp_column` only resolves once the parser has flattened.
[[nodiscard]] std::string parserConfigJson(
    std::string_view timestamp_field, std::int64_t synthetic_interval_ns,
    PJ::TimeUnit unit = PJ::TimeUnit::kNanoseconds);

}  // namespace mosaico
