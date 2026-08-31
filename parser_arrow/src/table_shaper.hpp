#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/arrow.hpp"
#include "pj_plugins/sdk/parser_array_policy.hpp"

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
  /// Cross-parser maximum expanded-list width and clamp/skip policy.
  PJ::sdk::ArrayLimit array_limit;
};

/// One source column removed because its final type is not ingestible by the current PlotJuggler host.
struct DroppedColumn {
  /// Final output name after flattening and empty-name substitution.
  std::string name;
  /// Final Arrow C Data format after timestamp casts and variable-width normalization.
  std::string format;
};

/// One primitive list expanded into a fixed set of scalar output columns.
struct ExpandedList {
  /// Final flattened source name before the bracketed element suffix.
  std::string name;
  /// Output width after applying the first-batch rule and ArrayLimit.
  int64_t width = 0;
  /// Whether ArrayLimit truncated the observed or schema-declared width.
  bool clamped = false;
};

/// Result of lazy shaping, including the int64-nanosecond timestamp column passed to the host.
struct ShapedStream {
  /// Lazy stream; validation-only plans move each original record-batch root through unchanged.
  PJ::sdk::ArrowStreamHolder stream;
  /// Resolved timestamp column name; never empty on success.
  std::string timestamp_column;
  /// Whether timestamp_ns was synthesized and prepended.
  bool synthesized_timestamp = false;
  /// Unsupported source columns removed from the shaped stream and reported to the caller.
  std::vector<DroppedColumn> dropped_columns;
  /// Primitive lists expanded by the resolved stream plan.
  std::vector<ExpandedList> expanded_lists;
};

/// Observable summary of timestamp resolution, rewrites, and removed unsupported columns.
struct ShapePlan {
  /// Whether output schemas or arrays change beyond validation-only timestamp scanning.
  bool needs_rewrite = false;
  /// Explicit, detected, or synthesized timestamp column name.
  std::string timestamp_column;
  /// Whether the rewrite prepends a synthetic timestamp_ns column.
  bool synthesize_timestamp = false;
  /// Unsupported source columns that will be removed from the shaped stream.
  std::vector<DroppedColumn> dropped_columns;
  /// Primitive fixed-size lists, plus unresolved variable lists with width zero in schema-only plans.
  std::vector<ExpandedList> expanded_lists;
};

/// Detect the first child whose format starts with "ts", otherwise the first child named timestamp_ns,
/// recording_timestamp_ns, timestamp, time, or ts in that priority order; return an empty string if none match.
[[nodiscard]] std::string detectTimestampColumn(const ArrowSchema* schema);

/// Inspect a decoded stream schema and select its timestamp and required rewrites.
///
/// Explicit timestamp names must exist. Resolved timestamps accept int32/int64/uint32/uint64 ticks, float/double
/// seconds, or Arrow timestamps; shapeStream() emits each as non-null int64 nanoseconds. Every Arrow timestamp
/// column is scaled to int64 nanoseconds. String/binary views and large string/binary are normalized to u/z.
/// Fixed-size primitive lists expand from schema; schema-only plans record variable primitive lists with unresolved
/// width zero, which shapeStream() replaces with the width measured from its buffered first batch. Unsupported final
/// data columns are removed and listed in dropped_columns; a schema with no host-ingestible data fails.
[[nodiscard]] PJ::Expected<ShapePlan> planShape(const ArrowSchema* schema, const ShapeOptions& options);

/// Lazily rewrite a decoded IPC stream while moving untouched columns and copying only casts, normalizations, and
/// flattened leaves that require ancestor validity or offset handling.
[[nodiscard]] PJ::Expected<ShapedStream> shapeStream(PJ::sdk::ArrowStreamHolder input, const ShapeOptions& options);

}  // namespace pj::parser_arrow
