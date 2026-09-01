// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

// Framing helpers for the `arrow-ipc` delegated-ingest encoding: one Flight
// record batch becomes one self-contained Arrow IPC stream (schema + batch +
// EOS), which is exactly what `parser_arrow` decodes. Plain libarrow, no SDK.

#include <arrow/result.h>
#include <arrow/type_fwd.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mosaico {

/// Cadence of the synthetic time axis for rows without a timestamp column
/// (~30 fps). Passed to parser_arrow as `synthetic_interval_ns`; the message
/// host timestamp advances by it per row so the parser continues the axis
/// seamlessly across batches.
inline constexpr std::int64_t kSyntheticIntervalNs = 33'333'333LL;

/// Rewrite @p schema into one parser_arrow can decode, or fail naming the
/// offending field path and type. An ALLOWLIST of what nanoarrow_ipc 0.7
/// accepts: view types collapse to their materialized form, dictionary and
/// extension fields to the type underneath, containers recurse — and ANYTHING
/// ELSE IS AN ERROR. The refusal is the point: a silent pass-through surfaces
/// later as an opaque host-side decode failure with no field to chase. Run-end
/// encoding and unions needing a rewrite are refused rather than cast, because
/// Arrow 23 has no kernel for either.
///
/// Returns the SAME pointer when nothing changes, so callers skip the cast pass
/// (and stay zero-copy) with a pointer compare. This side only asserts the
/// allowlist; that nanoarrow really decodes the result is parser_arrow's test
/// suite to prove.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Schema>> ipcSafeSchema(const std::shared_ptr<arrow::Schema>& schema);

/// Cast every column whose type differs from `target` (see ipcSafeSchema).
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::RecordBatch>> castToSchema(
    const arrow::RecordBatch& batch, const std::shared_ptr<arrow::Schema>& target);

/// A topic's timestamp leaf, named the way BOTH consumers name it: struct
/// children expand depth-first into `parent/child`, and every literal '.' in a
/// component becomes '/' (`wheel.speed` -> `wheel/speed`; `header.stamp` inside
/// struct `msg` -> `msg/header/stamp`). Same output as flattenStructColumns, so
/// one name serves the object helpers and parser_arrow alike.
struct TimestampLeaf {
  std::string path;        ///< empty when the schema has no timestamp leaf
  std::vector<int> route;  ///< top-level column index, then one struct-child index per level
};

/// Pick the timestamp leaf: the first Arrow TIMESTAMP-typed one in schema
/// order, else the first whose full path equals one of {timestamp_ns,
/// recording_timestamp_ns, timestamp, time, ts} — NAME-LIST order wins over
/// schema order, as it has since PJ3. Passed explicitly at bind time so the
/// layout records the policy (D9).
///
/// Mirror of parser_arrow's detectTimestampColumn
/// (parser_arrow/src/table_shaper.hpp) — keep the name lists in sync. The two
/// namings diverge on exactly one case, unreachable for a leaf a timestamp name
/// could match: an EMPTY field-name component, where parser_arrow substitutes
/// `_<index>` and Table::Flatten leaves a trailing `parent/`.
[[nodiscard]] TimestampLeaf detectTimestampLeaf(const arrow::Schema& schema);

/// Row 0 of the leaf at @p route as nanoseconds: floats are seconds, TIMESTAMP
/// is rescaled by its unit, integers of any width are already ns. Empty for an
/// empty route (how a timestamp-less topic is marked), an empty batch, a null
/// value or null ancestor, or a value that does not cast safely. Only the
/// message's host timestamp — the parser reads per-row time from the column.
[[nodiscard]] std::optional<std::int64_t> firstRowTimestampNs(
    const arrow::RecordBatch& batch, const std::vector<int>& route);

/// One record batch as a complete IPC stream (schema message, the batch, end
/// of stream). Uncompressed: the bytes cross a process-internal seam.
/// @p capacity_hint sizes the output buffer up front (the previous batch's
/// size is a good guess); 0 falls back to a small default.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Buffer>> serializeIpcStream(
    const arrow::RecordBatch& batch, std::int64_t capacity_hint = 0);

/// `parser_arrow` configuration for one topic: `{"timestamp_column": <leaf
/// path>, "synthetic_interval_ns": <interval>, "flatten_structs": true}`.
///
/// The interval is per-topic, not a constant: a timestamp-less topic spreads its
/// rows over the topic's [min,max] range, so the caller passes the fitted value.
/// `flatten_structs` is pinned rather than left to the parser's default because
/// a nested `timestamp_column` only resolves once the parser has flattened.
[[nodiscard]] std::string parserConfigJson(std::string_view timestamp_field, std::int64_t synthetic_interval_ns);

}  // namespace mosaico
