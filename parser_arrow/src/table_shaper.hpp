#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/arrow.hpp"

namespace pj::parser_arrow {

/// Options controlling how a decoded IPC stream is rewritten before host ingest.
struct ShapeOptions {
  /// Explicit timestamp column name; an empty value enables automatic detection.
  std::string timestamp_column;
  /// Flatten nested struct columns to slash-separated leaves; unflattened structs are reported as dropped.
  bool flatten_structs = true;
  /// Anchor in nanoseconds for a synthesized timestamp column.
  int64_t message_timestamp_ns = 0;
  /// Nanoseconds added for each row in a synthesized timestamp column.
  int64_t synthetic_interval_ns = 0;
};

/// Result of lazy shaping, including the int64-nanosecond timestamp column Task 4 must pass to the host.
struct ShapedStream {
  /// Lazy stream; validation-only plans move each original record-batch root through unchanged.
  PJ::sdk::ArrowStreamHolder stream;
  /// Resolved timestamp column name; never empty on success.
  std::string timestamp_column;
  /// Whether timestamp_ns was synthesized and prepended.
  bool synthesized_timestamp = false;
};

/// One final output column whose type the current PlotJuggler host silently skips.
struct DroppedColumn {
  /// Final output name after flattening and empty-name substitution.
  std::string name;
  /// Final Arrow C Data format after timestamp casts and variable-width normalization.
  std::string format;
};

/// Observable summary of timestamp resolution, rewrites, and host-skipped final columns.
struct ShapePlan {
  /// Whether output schemas or arrays change beyond validation-only timestamp scanning.
  bool needs_rewrite = false;
  /// Explicit, detected, or synthesized timestamp column name.
  std::string timestamp_column;
  /// Whether the rewrite prepends a synthetic timestamp_ns column.
  bool synthesize_timestamp = false;
  /// Final output columns retained in the stream even though the current host skips them.
  std::vector<DroppedColumn> dropped_columns;
};

/// Detect the first child whose format starts with "ts", otherwise the first child named timestamp_ns,
/// recording_timestamp_ns, timestamp, time, or ts in that priority order; return an empty string if none match.
[[nodiscard]] std::string detectTimestampColumn(const ArrowSchema* schema);

/// Inspect a decoded stream schema and select its timestamp and required rewrites.
///
/// Explicit timestamp names must exist. Resolved timestamps accept int32/int64/uint32/uint64 ticks, float/double
/// seconds, or Arrow timestamps; shapeStream() emits each as non-null int64 nanoseconds. Every Arrow timestamp
/// column is scaled to int64 nanoseconds. String/binary views and large string/binary are normalized to u/z.
/// Unsupported final data columns are listed in dropped_columns, and a schema with no host-ingestible data fails.
[[nodiscard]] PJ::Expected<ShapePlan> planShape(const ArrowSchema* schema, const ShapeOptions& options);

/// Lazily rewrite a decoded IPC stream while moving untouched columns and copying only casts, normalizations, and
/// flattened leaves that require ancestor validity or offset handling.
[[nodiscard]] PJ::Expected<ShapedStream> shapeStream(PJ::sdk::ArrowStreamHolder input, const ShapeOptions& options);

}  // namespace pj::parser_arrow
