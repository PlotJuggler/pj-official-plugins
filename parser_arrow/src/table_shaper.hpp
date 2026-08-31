#pragma once

#include <cstdint>
#include <string>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/arrow.hpp"

namespace pj::parser_arrow {

/// Options controlling how a decoded IPC stream is rewritten before ingest.
struct ShapeOptions {
  /// Explicit timestamp column name; an empty value enables automatic detection.
  std::string timestamp_column;
  /// Flatten nested struct columns to slash-separated primitive leaves.
  bool flatten_structs = true;
  /// Anchor in nanoseconds for a synthesized timestamp column.
  int64_t message_timestamp_ns = 0;
  /// Nanoseconds added for each row in a synthesized timestamp column.
  int64_t synthetic_interval_ns = 0;
};

/// Result of shaping, including the timestamp column Task 4 must pass to the host.
struct ShapedStream {
  /// Rewritten stream, or the original stream moved through when no rewrite is needed.
  PJ::sdk::ArrowStreamHolder stream;
  /// Resolved timestamp column name; never empty on success.
  std::string timestamp_column;
  /// Whether timestamp_ns was synthesized and prepended.
  bool synthesized_timestamp = false;
};

/// Observable summary of the schema rewrite selected by planShape().
struct ShapePlan {
  /// Whether shapeStream() must wrap and rewrite the input stream.
  bool needs_rewrite = false;
  /// Explicit, detected, or synthesized timestamp column name.
  std::string timestamp_column;
  /// Whether the rewrite prepends a synthetic timestamp_ns column.
  bool synthesize_timestamp = false;
};

/// Detect the first child whose format starts with "ts", otherwise the first child named timestamp_ns,
/// recording_timestamp_ns, timestamp, time, or ts in that priority order; return an empty string if none match.
[[nodiscard]] std::string detectTimestampColumn(const ArrowSchema* schema);

/// Inspect a decoded stream schema and select its timestamp and required rewrites.
[[nodiscard]] PJ::Expected<ShapePlan> planShape(const ArrowSchema* schema, const ShapeOptions& options);

/// Lazily rewrite a decoded IPC stream into the flat schema supported by the PlotJuggler host.
[[nodiscard]] PJ::Expected<ShapedStream> shapeStream(PJ::sdk::ArrowStreamHolder input, const ShapeOptions& options);

}  // namespace pj::parser_arrow
