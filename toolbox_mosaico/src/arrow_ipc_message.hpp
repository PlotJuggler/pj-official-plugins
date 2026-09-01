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

namespace mosaico {

/// Cadence of the synthetic time axis for rows without a timestamp column
/// (~30 fps). Passed to parser_arrow as `synthetic_interval_ns`; the message
/// host timestamp advances by it per row so the parser continues the axis
/// seamlessly across batches.
inline constexpr std::int64_t kSyntheticIntervalNs = 33'333'333LL;

/// The schema parser_arrow can decode: string_view/binary_view become
/// utf8/binary and dictionary-encoded fields become their value type, at any
/// nesting depth (struct children, list values) — nanoarrow_ipc 0.7 refuses
/// both. Returns the SAME pointer when nothing needs to change, so callers can
/// skip the cast pass (and stay zero-copy) with a pointer compare.
[[nodiscard]] std::shared_ptr<arrow::Schema> ipcSafeSchema(const std::shared_ptr<arrow::Schema>& schema);

/// Cast every column whose type differs from `target` (see ipcSafeSchema).
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::RecordBatch>> castToSchema(
    const arrow::RecordBatch& batch, const std::shared_ptr<arrow::Schema>& target);

/// Timestamp field of a topic schema: the first Arrow TIMESTAMP-typed field,
/// else the first of {timestamp_ns, recording_timestamp_ns, timestamp, time,
/// ts}; empty when nothing matches. Same rule `parser_arrow` applies, passed
/// explicitly at bind time so the layout records the policy (D9).
///
/// Mirror of parser_arrow's detectTimestampColumn
/// (parser_arrow/src/table_shaper.hpp) — keep the name lists in sync.
[[nodiscard]] std::string detectTimestampField(const arrow::Schema& schema);

/// Row 0 of column @p index as nanoseconds: floating values are read as
/// seconds, TIMESTAMP is rescaled to ns, anything else is cast to int64 and
/// taken as ns. Empty for an out-of-range index, an empty batch, a null or
/// non-finite value, or a value that does not cast safely. Used only as the
/// message's host timestamp — the parser reads per-row time from the column.
[[nodiscard]] std::optional<std::int64_t> firstRowTimestampNs(const arrow::RecordBatch& batch, int index);

/// One record batch as a complete IPC stream (schema message, the batch, end
/// of stream). Uncompressed: the bytes cross a process-internal seam.
/// @p capacity_hint sizes the output buffer up front (the previous batch's
/// size is a good guess); 0 falls back to a small default.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Buffer>> serializeIpcStream(
    const arrow::RecordBatch& batch, std::int64_t capacity_hint = 0);

/// `parser_arrow` configuration for one topic:
/// `{"timestamp_column": <field>, "synthetic_interval_ns": <interval>}`.
[[nodiscard]] std::string parserConfigJson(std::string_view timestamp_field, std::int64_t synthetic_interval_ns);

}  // namespace mosaico
