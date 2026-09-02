#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/arrow.hpp"
#include "pj_plugins/sdk/parser_array_policy.hpp"

namespace pj::parser_arrow {

/// Options controlling how a decoded IPC stream is rewritten before host ingest.
struct ShapeOptions {
  /// Explicit timestamp column, named as a flattened leaf at any depth; empty enables detection. Dots normalize to
  /// '/' as they do in leaf names, so `header.stamp` and `header/stamp` are the same request.
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

/// One source column omitted while planning a host-compatible output schema.
struct DroppedColumn {
  /// Final output name after flattening and empty-name substitution.
  std::string name;
  /// Final Arrow C Data format; a zero-width first-batch list carries an `(empty)` diagnostic marker.
  std::string format;
};

/// Keep fatal planning errors and runtime dropped-column diagnostics equally bounded.
inline constexpr std::size_t kMaxDroppedColumnsListed = 8;

/// One non-fatal condition fixed while planning the shaped stream.
struct ShapeWarning {
  std::string code;
  std::string message;
};

/// Facts discovered only while the host synchronously drains the shaped stream.
///
/// Plain members are safe because appendArrowStream drains on its calling thread; the shared ownership exists only
/// so these facts survive the stream release callback and can be inspected after that call returns.
struct RuntimeStats {
  int64_t rows_truncated = 0;
  std::string first_truncated_column;
  bool float_axis_magnitude_exceeded = false;
  std::string float_axis_column;
};

/// Result of lazy shaping, including the resolved int64-nanosecond timestamp column passed to the host.
struct ShapedStream {
  /// Lazy wrapper stream; untouched child arrays are moved without copying.
  PJ::sdk::ArrowStreamHolder stream;
  /// Resolved timestamp column name; never empty on success.
  std::string timestamp_column;
  /// Source columns omitted from the shaped stream and reported to the caller.
  std::vector<DroppedColumn> dropped_columns;
  /// Non-fatal diagnostics determined before the output schema is exposed.
  std::vector<ShapeWarning> warnings;
  /// True when the timestamp column is generated rather than read from the input.
  bool synthetic_axis = false;
  /// Drain-time facts shared with the lazy stream state.
  std::shared_ptr<RuntimeStats> runtime;
};

/// Lazily rewrite a decoded IPC stream while moving untouched columns and copying only casts, normalizations, and
/// flattened leaves that require ancestor validity or offset handling. Variable primitive-list widths are measured
/// from one buffered first batch before the output schema is exposed. Unsupported columns are returned for caller
/// diagnostics, and schemas without a host-ingestible non-axis data column are rejected.
[[nodiscard]] PJ::Expected<ShapedStream> shapeStream(PJ::sdk::ArrowStreamHolder input, const ShapeOptions& options);

/// Format at most `max_listed` dropped name/format pairs separated by comma-space, appending an ellipsis when more
/// columns were omitted. An empty input produces an empty string.
[[nodiscard]] std::string formatDroppedColumns(const std::vector<DroppedColumn>& columns, std::size_t max_listed);

}  // namespace pj::parser_arrow
