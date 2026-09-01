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

/// The schema parser_arrow can decode, as an ALLOWLIST of what nanoarrow_ipc
/// 0.7 actually accepts. At any nesting depth (struct/list/map children):
/// string_view -> utf8, binary_view -> binary, dictionary -> its value type,
/// list_view -> list, large_list_view -> large_list; struct / list / large_list
/// / fixed_size_list / map recurse into their children. Everything else is
/// either decodable as-is or an ERROR naming the field path and type — there is
/// no silent pass-through, because a refused type only surfaces later as an
/// opaque decode failure host-side. Run-end-encoded and any union whose child
/// needs rewriting are errors: Arrow 23 has no cast kernel for them.
///
/// Returns the SAME pointer when nothing needs to change, so callers can skip
/// the cast pass (and stay zero-copy) with a pointer compare.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Schema>> ipcSafeSchema(const std::shared_ptr<arrow::Schema>& schema);

/// Cast every column whose type differs from `target` (see ipcSafeSchema).
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::RecordBatch>> castToSchema(
    const arrow::RecordBatch& batch, const std::shared_ptr<arrow::Schema>& target);

/// Timestamp leaf of a topic schema, as the FLATTENED leaf path both sides
/// agree on: struct children are expanded depth-first into `parent/child`, and
/// every literal '.' inside a name component becomes '/' (`wheel.speed` ->
/// `wheel/speed`; `header.stamp` inside struct `msg` -> `msg/header/stamp`).
/// That is exactly what flattenStructColumns produces, so the name reaches the
/// object helpers and parser_arrow unchanged.
///
/// Over those leaves, in order: the first Arrow TIMESTAMP-typed one, else the
/// first path equal to one of {timestamp_ns, recording_timestamp_ns, timestamp,
/// time, ts} (name-list order wins, not schema order — as it has since PJ3);
/// empty when nothing matches. Passed explicitly at bind time so the layout
/// records the policy (D9).
///
/// Mirror of parser_arrow's detectTimestampColumn
/// (parser_arrow/src/table_shaper.hpp) — keep the name lists in sync.
[[nodiscard]] std::string detectTimestampField(const arrow::Schema& schema);

/// Child-index route to the leaf whose flattened path is @p leaf_path: the
/// top-level column index followed by one struct-child index per level. Empty
/// when the path does not resolve (an empty @p leaf_path included), which is
/// how a timestamp-less topic is marked. Resolving by walk rather than by
/// splitting on '/' is load-bearing: a component may itself contain a '/' that
/// came from a '.' in the field name.
[[nodiscard]] std::vector<int> timestampFieldRoute(const arrow::Schema& schema, std::string_view leaf_path);

/// Row 0 of the leaf reached by @p route (see timestampFieldRoute) as
/// nanoseconds: floating values are read as seconds, TIMESTAMP is rescaled to
/// ns, anything else is cast to int64 and taken as ns. Empty for an empty
/// route, an empty batch, a null or non-finite value, or a value that does not
/// cast safely. Used only as the message's host timestamp — the parser reads
/// per-row time from the column.
[[nodiscard]] std::optional<std::int64_t> firstRowTimestampNs(
    const arrow::RecordBatch& batch, const std::vector<int>& route);

/// One record batch as a complete IPC stream (schema message, the batch, end
/// of stream). Uncompressed: the bytes cross a process-internal seam.
/// @p capacity_hint sizes the output buffer up front (the previous batch's
/// size is a good guess); 0 falls back to a small default.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Buffer>> serializeIpcStream(
    const arrow::RecordBatch& batch, std::int64_t capacity_hint = 0);

/// `parser_arrow` configuration for one topic:
/// `{"timestamp_column": <leaf path>, "synthetic_interval_ns": <interval>}`.
/// The interval is per-topic, not a constant: a timestamp-less topic spreads its
/// rows over the topic's [min,max] range, so the caller passes the fitted value.
[[nodiscard]] std::string parserConfigJson(
    std::string_view timestamp_field, std::int64_t synthetic_interval_ns = kSyntheticIntervalNs);

}  // namespace mosaico
