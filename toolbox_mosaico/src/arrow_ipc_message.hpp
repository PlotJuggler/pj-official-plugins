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
/// (~30 fps), stored in the IPC payload when no source timestamp is available.
inline constexpr std::int64_t kSyntheticIntervalNs = PJ::kDefaultSyntheticIntervalNs;

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
/// Use the schema and empty-name convention the consumer will see:
/// kIndex for scalars, raw with kFlatten for the object route's Table::Flatten.
[[nodiscard]] TimestampLeaf detectTimestampLeaf(
    const arrow::Schema& schema, EmptyNameRule empty_name_rule, PJ::TimeUnit unit = PJ::TimeUnit::kNanoseconds);

/// Row 0 of the leaf at @p route as nanoseconds: FLOAT/DOUBLE are seconds,
/// TIMESTAMP is rescaled by its native unit, integers use @p unit (default ns).
/// Empty for an empty route (how a timestamp-less topic is marked), an empty
/// batch, a null value or null ancestor, or a value that does not convert
/// safely. Half-float is deliberately NOT accepted — parser_arrow refuses it as
/// an axis, so stamping from it here would disagree with the parser.
///
/// Only the message's host timestamp — the parser reads per-row time from the
/// column. Pass the source batch whose schema produced the route.
[[nodiscard]] std::optional<std::int64_t> firstRowTimestampNs(
    const arrow::RecordBatch& batch, const std::vector<int>& route, PJ::TimeUnit unit = PJ::TimeUnit::kNanoseconds);

/// Preserve every source column and prepend a collision-free native timestamp
/// column. Persisting the fitted cadence makes replay independent of parser config.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::RecordBatch>> addSyntheticTimestamps(
    const arrow::RecordBatch& batch, std::int64_t anchor, std::int64_t interval, std::int64_t row_offset);

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
