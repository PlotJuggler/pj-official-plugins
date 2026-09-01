#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/arrow.hpp"
#include "pj_plugins/sdk/parser_array_policy.hpp"

namespace pj::parser_arrow {

/// Options controlling how a decoded IPC stream is rewritten before host ingest.
struct ShapeOptions {
  /// Explicit timestamp column, named as a flattened leaf (`header/stamp`) at any depth; empty enables detection.
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

/// Result of lazy shaping, including the resolved int64-nanosecond timestamp column passed to the host.
struct ShapedStream {
  /// Lazy wrapper stream; untouched child arrays are moved without copying.
  PJ::sdk::ArrowStreamHolder stream;
  /// Resolved timestamp column name; never empty on success.
  std::string timestamp_column;
  /// Unsupported source columns removed from the shaped stream and reported to the caller.
  std::vector<DroppedColumn> dropped_columns;
};

/// Detect a timestamp axis among the flattened leaf names of `schema` (structs flattened, dots normalized to '/'):
/// the first Arrow TIMESTAMP-typed leaf in schema order, otherwise the first leaf named timestamp_ns,
/// recording_timestamp_ns, timestamp, time, or ts in that priority order; empty when nothing matches.
[[nodiscard]] std::string detectTimestampColumn(const ArrowSchema* schema);

/// Lazily rewrite a decoded IPC stream while moving untouched columns and copying only casts, normalizations, and
/// flattened leaves that require ancestor validity or offset handling. Variable primitive-list widths are measured
/// from one buffered first batch before the output schema is exposed. Unsupported columns are returned for caller
/// diagnostics, and schemas without a host-ingestible non-axis data column are rejected.
[[nodiscard]] PJ::Expected<ShapedStream> shapeStream(PJ::sdk::ArrowStreamHolder input, const ShapeOptions& options);

/// Format at most `max_listed` dropped name/format pairs separated by comma-space. An empty input produces an empty
/// string. Callers retain their existing policy for indicating additional omitted columns.
[[nodiscard]] std::string formatDroppedColumns(const std::vector<DroppedColumn>& columns, std::size_t max_listed);

}  // namespace pj::parser_arrow
