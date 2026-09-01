// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

// Framing helpers for the `arrow-ipc` delegated-ingest encoding: one Flight
// record batch becomes one self-contained Arrow IPC stream (schema + batch +
// EOS), which is exactly what `parser_arrow` decodes. Plain libarrow, no SDK.

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mosaico {

/// Timestamp field of a topic schema: the first Arrow TIMESTAMP-typed field,
/// else the first of {timestamp_ns, recording_timestamp_ns, timestamp, time,
/// ts}; empty when nothing matches. Same rule `parser_arrow` applies, passed
/// explicitly at bind time so the layout records the policy (D9).
[[nodiscard]] std::string detectTimestampField(const arrow::Schema& schema);

/// Row 0 of `field` as nanoseconds: integers are taken as ns, floating values
/// as seconds, TIMESTAMP scaled by its unit. Empty when the batch is empty,
/// the field is absent, the value is null/non-finite, or the type is not a
/// time-like scalar. Used only as the message's host timestamp — the parser
/// reads per-row time from the column itself.
[[nodiscard]] std::optional<std::int64_t> firstRowTimestampNs(const arrow::RecordBatch& batch, std::string_view field);

/// One record batch as a complete IPC stream (schema message, the batch, end
/// of stream). Uncompressed: the bytes cross a process-internal seam.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Buffer>> serializeIpcStream(const arrow::RecordBatch& batch);

/// The schema alone as an IPC schema message — the binding's informational
/// `schema` bytes (a recording keeps them; `parser_arrow` reads the stream).
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Buffer>> serializeIpcSchema(const arrow::Schema& schema);

/// `parser_arrow` configuration for one topic:
/// `{"timestamp_column": <field>, "synthetic_interval_ns": <interval>}`.
[[nodiscard]] std::string parserConfigJson(std::string_view timestamp_field, std::int64_t synthetic_interval_ns);

}  // namespace mosaico
