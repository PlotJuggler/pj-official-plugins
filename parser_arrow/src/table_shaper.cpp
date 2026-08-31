#include "table_shaper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pj::parser_arrow {
namespace {

/// Copy, cast, validation, or generation applied to one output column.
enum class CastKind {
  kNone,
  kSyntheticTimestamp,
  kNormalizeBytes,
  kTimestampSecond,
  kTimestampMilli,
  kTimestampMicro,
  kTimestampNano,
  kTimestampAxisSecond,
  kTimestampAxisMilli,
  kTimestampAxisMicro,
  kTimestampAxisNano,
  kTimestampAxisInt32,
  kTimestampAxisInt64,
  kTimestampAxisUint32,
  kTimestampAxisUint64,
  kTimestampAxisFloatSeconds,
  kTimestampAxisDoubleSeconds,
  kListElement,
};

/// One output column and the source path or cast used to populate it.
struct OutputColumn {
  /// Child indices from the record-batch root to the source leaf.
  std::vector<int64_t> source_path;
  /// Value transformation; kNone permits an ownership move when parent layout allows it.
  CastKind cast = CastKind::kNone;
  /// Scalar transformation applied after kListElement resolves its child value.
  CastKind element_cast = CastKind::kNone;
  /// Zero-based child position populated by kListElement.
  int64_t element_index = -1;
  /// Fixed child count for fixed_size_list, or zero for offset-based lists.
  int32_t fixed_list_size = 0;
  /// Schema-only placeholder for a variable list whose first-batch width is unresolved.
  bool deferred_variable_list = false;
  /// List omitted because its width is zero or ArrayLimit selected skip.
  bool force_drop = false;
  /// Diagnostic format override for an empty list expansion.
  std::string dropped_format;
  /// Whether flattening removed a nullable struct ancestor from this leaf.
  bool nullable_struct_ancestor = false;
  /// Final output name, retained for callback diagnostics.
  std::string name;
  /// Logical source type, used to reject unsupported complex-value copies clearly.
  ArrowType source_type = NANOARROW_TYPE_UNINITIALIZED;
  /// Decimal parameters used by ArrowArrayViewGetDecimalUnsafe and ArrowArrayAppendDecimal.
  int32_t decimal_bitwidth = 0;
  int32_t decimal_precision = 0;
  int32_t decimal_scale = 0;
};

/// Schema and column mapping used by the lazy wrapping stream.
struct RewritePlan {
  PJ::sdk::ArrowSchemaHolder output_schema;
  std::vector<OutputColumn> columns;
  std::vector<DroppedColumn> dropped_columns;
  std::vector<ExpandedList> expanded_lists;
  bool needs_rewrite = false;
  int64_t timestamp_source_index = -1;
};

/// Public planning result paired with the one internal schema rewrite it describes.
struct CompleteShapePlan {
  ShapePlan shape;
  RewritePlan rewrite;
};

/// State owned by one lazy shaping ArrowArrayStream.
struct ShapingStreamState {
  PJ::sdk::ArrowStreamHolder input;
  PJ::sdk::ArrowSchemaHolder input_schema;
  PJ::sdk::ArrowSchemaHolder output_schema;
  PJ::sdk::ArrowArrayHolder pending_first_batch;
  std::vector<OutputColumn> columns;
  bool rewrite_batches = true;
  int64_t timestamp_source_index = -1;
  std::string timestamp_column;
  int64_t message_timestamp_ns = 0;
  int64_t synthetic_interval_ns = 0;
  int64_t row_offset = 0;
  bool input_at_eos = false;
  int poisoned_error_code = NANOARROW_OK;
  std::string last_error;
  const char* fixed_error = nullptr;
};

constexpr char kCallbackErrorFallback[] = "parser_arrow: unable to store Arrow stream error details";

/// Width resolved for one ingestible variable list from the first record batch.
struct VariableListWidth {
  std::vector<int64_t> source_path;
  int64_t width = 0;
};

/// Schema path and flattened name of one ingestible variable list requiring a first-batch peek.
struct VariableListCandidate {
  std::vector<int64_t> source_path;
  std::string name;
};

/// Reset an ArrowArrayView held by a unique_ptr deleter.
void resetArrayView(ArrowArrayView* view) {
  ArrowArrayViewReset(view);
}

/// Prefix a nanoarrow diagnostic for the parser-facing error contract.
[[nodiscard]] std::string parserError(std::string_view message) {
  return "parser_arrow: " + std::string(message);
}

/// Convert an errno-style nanoarrow result into a parser-facing diagnostic.
[[nodiscard]] std::string nanoarrowError(int result, const char* message) {
  if (message != nullptr && message[0] != '\0') {
    return parserError(message);
  }
  return parserError(std::error_code(result, std::generic_category()).message());
}

/// Store a callback diagnostic without allowing allocation failure to cross the C ABI.
void setStreamError(ShapingStreamState& state, std::string_view message) noexcept {
  try {
    std::string detailed = "parser_arrow: ";
    detailed.append(message);
    state.last_error = std::move(detailed);
    state.fixed_error = nullptr;
  } catch (...) {
    state.fixed_error = kCallbackErrorFallback;
  }
}

/// Store a nanoarrow callback diagnostic with an errno fallback and no exception escape.
void setNanoarrowStreamError(ShapingStreamState& state, int result, const char* message) noexcept {
  if (message != nullptr && message[0] != '\0') {
    setStreamError(state, message);
    return;
  }
  try {
    const std::string errno_message = std::error_code(result, std::generic_category()).message();
    setStreamError(state, errno_message);
  } catch (...) {
    state.fixed_error = kCallbackErrorFallback;
  }
}

/// Return whether a schema represents an Arrow struct.
[[nodiscard]] bool isStruct(const ArrowSchema* schema) {
  return schema != nullptr && schema->format != nullptr && std::string_view(schema->format) == "+s";
}

/// Return whether a schema is normalized through the canonical byte-copy path.
[[nodiscard]] bool needsByteNormalization(const ArrowSchema* schema) {
  if (schema == nullptr || schema->format == nullptr) {
    return false;
  }
  const std::string_view format(schema->format);
  return format == "vu" || format == "vz" || format == "U" || format == "Z";
}

/// Return whether a top-level field with the exact requested name exists.
[[nodiscard]] bool hasColumn(const ArrowSchema* schema, std::string_view name) {
  if (schema == nullptr || schema->children == nullptr) {
    return false;
  }
  for (int64_t index = 0; index < schema->n_children; ++index) {
    const auto* child = schema->children[index];
    if (child != nullptr && child->name != nullptr && std::string_view(child->name) == name) {
      return true;
    }
  }
  return false;
}

/// Find a top-level child with the exact requested name.
[[nodiscard]] const ArrowSchema* findColumn(const ArrowSchema* schema, std::string_view name, int64_t* index_out) {
  if (schema == nullptr || schema->children == nullptr) {
    return nullptr;
  }
  for (int64_t index = 0; index < schema->n_children; ++index) {
    const auto* child = schema->children[index];
    if (child != nullptr && child->name != nullptr && std::string_view(child->name) == name) {
      *index_out = index;
      return child;
    }
  }
  return nullptr;
}

/// Return the stable output component for a nullable or empty child name.
[[nodiscard]] std::string outputNameComponent(const ArrowSchema* schema, int64_t child_index) {
  if (schema != nullptr && schema->name != nullptr && schema->name[0] != '\0') {
    return schema->name;
  }
  return "_" + std::to_string(child_index);
}

/// Copy a nullable C string into a nanoarrow-owned schema field.
[[nodiscard]] int copySchemaName(ArrowSchema* output, const char* name) {
  if (name == nullptr) {
    return NANOARROW_OK;
  }
  return ArrowSchemaSetName(output, name);
}

/// Copy nullable C Data schema metadata into nanoarrow-owned storage.
[[nodiscard]] int copySchemaMetadata(ArrowSchema* output, const char* metadata) {
  if (metadata == nullptr) {
    return NANOARROW_OK;
  }
  return ArrowSchemaSetMetadata(output, metadata);
}

/// Resolve a child schema by a path known to have been collected from the same root.
[[nodiscard]] const ArrowSchema* schemaAtPath(const ArrowSchema* root, const std::vector<int64_t>& path) {
  const ArrowSchema* current = root;
  for (const int64_t child_index : path) {
    current = current->children[child_index];
  }
  return current;
}

/// Return whether a cast marks the resolved time axis and therefore rejects nulls.
[[nodiscard]] bool isTimestampAxisCast(CastKind cast) {
  return cast >= CastKind::kTimestampAxisSecond && cast <= CastKind::kTimestampAxisDoubleSeconds;
}

/// Return whether a source-backed output is represented as int64 after shaping.
[[nodiscard]] bool castsToInt64(CastKind cast) {
  return (cast >= CastKind::kTimestampSecond && cast <= CastKind::kTimestampAxisDoubleSeconds);
}

/// Select the cast for one Arrow timestamp unit.
[[nodiscard]] CastKind timestampCast(ArrowTimeUnit unit, bool is_axis) {
  switch (unit) {
    case NANOARROW_TIME_UNIT_SECOND:
      return is_axis ? CastKind::kTimestampAxisSecond : CastKind::kTimestampSecond;
    case NANOARROW_TIME_UNIT_MILLI:
      return is_axis ? CastKind::kTimestampAxisMilli : CastKind::kTimestampMilli;
    case NANOARROW_TIME_UNIT_MICRO:
      return is_axis ? CastKind::kTimestampAxisMicro : CastKind::kTimestampMicro;
    case NANOARROW_TIME_UNIT_NANO:
      return is_axis ? CastKind::kTimestampAxisNano : CastKind::kTimestampNano;
  }
  return is_axis ? CastKind::kTimestampAxisNano : CastKind::kTimestampNano;
}

/// Parse a source schema and select its value transformation.
[[nodiscard]] PJ::Expected<CastKind> sourceCast(const ArrowSchema* schema, bool is_timestamp_axis) {
  if (schema == nullptr || schema->format == nullptr) {
    return PJ::unexpected("parser_arrow: malformed Arrow child schema");
  }
  ArrowSchemaView view{};
  ArrowError error{};
  const int result = ArrowSchemaViewInit(&view, schema, &error);
  if (result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(result, error.message));
  }
  if (view.type == NANOARROW_TYPE_TIMESTAMP) {
    return timestampCast(view.time_unit, is_timestamp_axis);
  }
  if (!is_timestamp_axis) {
    return needsByteNormalization(schema) ? CastKind::kNormalizeBytes : CastKind::kNone;
  }
  switch (view.type) {
    case NANOARROW_TYPE_INT32:
      return CastKind::kTimestampAxisInt32;
    case NANOARROW_TYPE_INT64:
      return CastKind::kTimestampAxisInt64;
    case NANOARROW_TYPE_UINT32:
      return CastKind::kTimestampAxisUint32;
    case NANOARROW_TYPE_UINT64:
      return CastKind::kTimestampAxisUint64;
    case NANOARROW_TYPE_FLOAT:
      return CastKind::kTimestampAxisFloatSeconds;
    case NANOARROW_TYPE_DOUBLE:
      return CastKind::kTimestampAxisDoubleSeconds;
    default:
      return PJ::unexpected(
          "parser_arrow: timestamp column '" + std::string(schema->name != nullptr ? schema->name : "") +
          "' has unsupported type '" + schema->format + "'");
  }
}

/// Populate source logical-type parameters needed by scalar reconstruction.
PJ::Status setSourceType(const ArrowSchema* schema, OutputColumn& column) {
  ArrowSchemaView view{};
  ArrowError error{};
  const int result = ArrowSchemaViewInit(&view, schema, &error);
  if (result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(result, error.message));
  }
  column.source_type = view.type;
  column.decimal_bitwidth = view.decimal_bitwidth;
  column.decimal_precision = view.decimal_precision;
  column.decimal_scale = view.decimal_scale;
  return PJ::okStatus();
}

/// Return whether the current PlotJuggler host imports a final output schema.
[[nodiscard]] PJ::Expected<bool> isHostIngestible(const ArrowSchema* schema) {
  ArrowSchemaView view{};
  ArrowError error{};
  const int result = ArrowSchemaViewInit(&view, schema, &error);
  if (result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(result, error.message));
  }
  switch (view.type) {
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64:
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DOUBLE:
    case NANOARROW_TYPE_BOOL:
    case NANOARROW_TYPE_STRING:
      return true;
    default:
      return false;
  }
}

/// Return whether a logical type uses Arrow list offsets or a schema-declared fixed width.
[[nodiscard]] bool isListType(ArrowType type) {
  return type == NANOARROW_TYPE_LIST || type == NANOARROW_TYPE_LARGE_LIST || type == NANOARROW_TYPE_FIXED_SIZE_LIST;
}

/// Return the scalar cast represented by an output column, including list-element columns.
[[nodiscard]] CastKind scalarCast(const OutputColumn& column) {
  return column.cast == CastKind::kListElement ? column.element_cast : column.cast;
}

/// Resolve a list schema view and reject malformed child layouts.
[[nodiscard]] PJ::Expected<ArrowSchemaView> listSchemaView(const ArrowSchema* schema) {
  ArrowSchemaView view{};
  ArrowError error{};
  const int result = ArrowSchemaViewInit(&view, schema, &error);
  if (result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(result, error.message));
  }
  if (!isListType(view.type) || schema->n_children != 1 || schema->children == nullptr ||
      schema->children[0] == nullptr) {
    return PJ::unexpected("parser_arrow: malformed Arrow list schema");
  }
  return view;
}

/// Return whether one list element becomes a host-ingestible scalar after existing normalizations.
[[nodiscard]] PJ::Expected<bool> isIngestibleListElement(const ArrowSchema* element, CastKind* cast_out) {
  auto cast = sourceCast(element, false);
  if (!cast) {
    return PJ::unexpected(std::move(cast).error());
  }
  *cast_out = *cast;
  if (*cast == CastKind::kNormalizeBytes) {
    const std::string_view format(element->format != nullptr ? element->format : "");
    return format == "vu" || format == "U";
  }
  if (castsToInt64(*cast)) {
    return true;
  }
  return isHostIngestible(element);
}

/// Collect ingestible variable lists that require a first-batch width measurement.
PJ::Status collectVariableListCandidates(
    const ArrowSchema* schema, const std::vector<int64_t>& path, std::string name, bool flatten_structs,
    std::vector<VariableListCandidate>& candidates) {
  if (schema == nullptr) {
    return PJ::unexpected("parser_arrow: malformed null Arrow child schema");
  }
  if (flatten_structs && isStruct(schema)) {
    for (int64_t child_index = 0; child_index < schema->n_children; ++child_index) {
      const auto* child = schema->children[child_index];
      auto child_path = path;
      child_path.push_back(child_index);
      auto status = collectVariableListCandidates(
          child, child_path, name + "/" + outputNameComponent(child, child_index), flatten_structs, candidates);
      if (!status) {
        return status;
      }
    }
    return PJ::okStatus();
  }

  ArrowSchemaView view{};
  ArrowError error{};
  const int result = ArrowSchemaViewInit(&view, schema, &error);
  if (result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(result, error.message));
  }
  if (view.type != NANOARROW_TYPE_LIST && view.type != NANOARROW_TYPE_LARGE_LIST) {
    return PJ::okStatus();
  }
  auto list_view = listSchemaView(schema);
  if (!list_view) {
    return PJ::unexpected(std::move(list_view).error());
  }
  CastKind element_cast = CastKind::kNone;
  auto ingestible = isIngestibleListElement(schema->children[0], &element_cast);
  if (!ingestible) {
    return PJ::unexpected(std::move(ingestible).error());
  }
  if (*ingestible) {
    candidates.push_back(VariableListCandidate{path, std::move(name)});
  }
  return PJ::okStatus();
}

/// Resolve a view and logical index while respecting offsets on every parent struct.
[[nodiscard]] const ArrowArrayView* viewAtPath(
    const ArrowArrayView* root, const std::vector<int64_t>& path, int64_t row, int64_t* leaf_row, bool* parent_is_null);

/// Resolve one list row's child bounds from the list view storage layout.
[[nodiscard]] bool listChildBounds(
    const ArrowArrayView* list, int64_t row, int32_t fixed_size, int64_t* begin, int64_t* end) {
  if (row < 0 || list->offset > std::numeric_limits<int64_t>::max() - row) {
    return false;
  }
  const int64_t physical_row = list->offset + row;
  if (fixed_size > 0) {
    if (physical_row > std::numeric_limits<int64_t>::max() / fixed_size) {
      return false;
    }
    *begin = physical_row * fixed_size;
    if (*begin > std::numeric_limits<int64_t>::max() - fixed_size) {
      return false;
    }
    *end = *begin + fixed_size;
    return true;
  }
  *begin = ArrowArrayViewListChildOffset(list, physical_row);
  *end = ArrowArrayViewListChildOffset(list, physical_row + 1);
  return *begin >= 0 && *end >= *begin;
}

/// Measure maximum row length for each candidate list in the buffered first batch.
[[nodiscard]] PJ::Expected<std::vector<VariableListWidth>> measureVariableListWidths(
    const ArrowSchema* schema, const ArrowArray* first_batch, const std::vector<VariableListCandidate>& candidates) {
  std::vector<VariableListWidth> widths;
  widths.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    widths.push_back(VariableListWidth{candidate.source_path, 0});
  }
  if (first_batch == nullptr) {
    return widths;
  }

  ArrowArrayView input_view{};
  const std::unique_ptr<ArrowArrayView, decltype(&resetArrayView)> view_owner(&input_view, &resetArrayView);
  ArrowError error{};
  int result = ArrowArrayViewInitFromSchema(&input_view, schema, &error);
  if (result == NANOARROW_OK) {
    result = ArrowArrayViewSetArray(&input_view, first_batch, &error);
  }
  if (result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(result, error.message));
  }

  for (std::size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
    for (int64_t row = 0; row < first_batch->length; ++row) {
      int64_t list_row = row;
      bool parent_is_null = false;
      const auto* list =
          viewAtPath(&input_view, candidates[candidate_index].source_path, row, &list_row, &parent_is_null);
      if (parent_is_null || ArrowArrayViewIsNull(list, list_row) != 0) {
        continue;
      }
      int64_t begin = 0;
      int64_t end = 0;
      if (!listChildBounds(list, list_row, 0, &begin, &end)) {
        return PJ::unexpected(
            "parser_arrow: invalid offsets in list column '" + candidates[candidate_index].name + "'");
      }
      widths[candidate_index].width = std::max(widths[candidate_index].width, end - begin);
    }
  }
  return widths;
}

/// Find a measured variable-list width by its schema path.
[[nodiscard]] const VariableListWidth* findVariableListWidth(
    const std::vector<VariableListWidth>& widths, const std::vector<int64_t>& path) {
  const auto found = std::find_if(
      widths.begin(), widths.end(), [&path](const VariableListWidth& width) { return width.source_path == path; });
  return found == widths.end() ? nullptr : &*found;
}

/// Collect depth-first output leaves, sanitized names, casts, and source paths.
PJ::Status collectOutputColumns(
    const ArrowSchema* schema, const std::vector<int64_t>& path, std::string name, bool flatten_structs,
    bool nullable_struct_ancestor, int64_t timestamp_source_index, const PJ::sdk::ArrayLimit& array_limit,
    const std::vector<VariableListWidth>& variable_widths, bool variable_widths_resolved,
    std::vector<ExpandedList>& expanded_lists, std::vector<OutputColumn>& output) {
  if (schema == nullptr) {
    return PJ::unexpected("parser_arrow: malformed null Arrow child schema");
  }
  if (flatten_structs && isStruct(schema)) {
    const bool descendants_nullable = nullable_struct_ancestor || (schema->flags & ARROW_FLAG_NULLABLE) != 0;
    for (int64_t child_index = 0; child_index < schema->n_children; ++child_index) {
      const auto* child = schema->children[child_index];
      auto child_path = path;
      child_path.push_back(child_index);
      std::string child_name = name + "/" + outputNameComponent(child, child_index);
      auto status = collectOutputColumns(
          child, child_path, std::move(child_name), flatten_structs, descendants_nullable, timestamp_source_index,
          array_limit, variable_widths, variable_widths_resolved, expanded_lists, output);
      if (!status) {
        return status;
      }
    }
    return PJ::okStatus();
  }

  ArrowSchemaView schema_view{};
  ArrowError schema_error{};
  const int schema_result = ArrowSchemaViewInit(&schema_view, schema, &schema_error);
  if (schema_result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(schema_result, schema_error.message));
  }
  if (isListType(schema_view.type)) {
    auto list_view = listSchemaView(schema);
    if (!list_view) {
      return PJ::unexpected(std::move(list_view).error());
    }
    CastKind element_cast = CastKind::kNone;
    auto ingestible_element = isIngestibleListElement(schema->children[0], &element_cast);
    if (!ingestible_element) {
      return PJ::unexpected(std::move(ingestible_element).error());
    }
    if (*ingestible_element) {
      int64_t real_width = 0;
      bool deferred = false;
      if (schema_view.type == NANOARROW_TYPE_FIXED_SIZE_LIST) {
        real_width = list_view->fixed_size;
      } else if (variable_widths_resolved) {
        const auto* measured = findVariableListWidth(variable_widths, path);
        if (measured == nullptr) {
          return PJ::unexpected("parser_arrow: missing first-batch width for list column '" + name + "'");
        }
        real_width = measured->width;
      } else {
        deferred = true;
      }

      int64_t output_width = real_width;
      bool clamped = false;
      const bool oversized = array_limit.max_size > 0 && output_width > static_cast<int64_t>(array_limit.max_size);
      if (oversized && !array_limit.clamp()) {
        OutputColumn column;
        column.source_path = path;
        column.force_drop = true;
        column.name = std::move(name);
        column.source_type = schema_view.type;
        output.push_back(std::move(column));
        return PJ::okStatus();
      }
      if (oversized) {
        output_width = static_cast<int64_t>(array_limit.max_size);
        clamped = true;
      }
      expanded_lists.push_back(ExpandedList{name, output_width, clamped});

      if (deferred) {
        OutputColumn column;
        column.source_path = path;
        column.deferred_variable_list = true;
        column.name = std::move(name);
        column.source_type = schema_view.type;
        output.push_back(std::move(column));
        return PJ::okStatus();
      }
      if (output_width == 0) {
        OutputColumn column;
        column.source_path = path;
        column.force_drop = true;
        column.name = std::move(name);
        column.source_type = schema_view.type;
        column.dropped_format = std::string(schema->format) + "(empty)";
        output.push_back(std::move(column));
        return PJ::okStatus();
      }

      for (int64_t element_index = 0; element_index < output_width; ++element_index) {
        OutputColumn column;
        column.source_path = path;
        column.cast = CastKind::kListElement;
        column.element_cast = element_cast;
        column.element_index = element_index;
        column.fixed_list_size =
            schema_view.type == NANOARROW_TYPE_FIXED_SIZE_LIST ? list_view->fixed_size : int32_t{0};
        column.nullable_struct_ancestor = nullable_struct_ancestor;
        column.name = name + "[" + std::to_string(element_index) + "]";
        auto type_status = setSourceType(schema->children[0], column);
        if (!type_status) {
          return type_status;
        }
        output.push_back(std::move(column));
      }
      return PJ::okStatus();
    }
  }

  OutputColumn column;
  column.source_path = path;
  const bool is_timestamp_axis = path.size() == 1 && path[0] == timestamp_source_index;
  auto cast = sourceCast(schema, is_timestamp_axis);
  if (!cast) {
    return PJ::unexpected(std::move(cast).error());
  }
  column.cast = *cast;
  auto type_status = setSourceType(schema, column);
  if (!type_status) {
    return type_status;
  }
  column.nullable_struct_ancestor = nullable_struct_ancestor;
  column.name = std::move(name);
  output.push_back(std::move(column));
  return PJ::okStatus();
}

/// Build the rewritten schema and output-to-input path mapping.
[[nodiscard]] PJ::Expected<RewritePlan> buildRewritePlan(
    const ArrowSchema* input_schema, const ShapeOptions& options, std::string_view timestamp_column,
    bool synthesize_timestamp, const std::vector<VariableListWidth>& variable_widths, bool variable_widths_resolved) {
  int64_t timestamp_source_index = -1;
  if (!synthesize_timestamp) {
    const ArrowSchema* timestamp_schema = findColumn(input_schema, timestamp_column, &timestamp_source_index);
    if (timestamp_schema == nullptr) {
      return PJ::unexpected(
          "parser_arrow: timestamp column '" + std::string(timestamp_column) + "' is absent from the Arrow schema");
    }
    auto timestamp_cast = sourceCast(timestamp_schema, true);
    if (!timestamp_cast) {
      return PJ::unexpected(std::move(timestamp_cast).error());
    }
  }

  RewritePlan rewrite;
  std::vector<OutputColumn> collected;
  if (synthesize_timestamp) {
    OutputColumn synthetic_column;
    synthetic_column.cast = CastKind::kSyntheticTimestamp;
    synthetic_column.name = "timestamp_ns";
    collected.push_back(std::move(synthetic_column));
  }

  for (int64_t child_index = 0; child_index < input_schema->n_children; ++child_index) {
    const auto* child = input_schema->children[child_index];
    std::vector<int64_t> path = {child_index};
    const std::string name = outputNameComponent(child, child_index);
    auto status = collectOutputColumns(
        child, path, name, options.flatten_structs, false, timestamp_source_index, options.array_limit, variable_widths,
        variable_widths_resolved, rewrite.expanded_lists, collected);
    if (!status) {
      return PJ::unexpected(std::move(status).error());
    }
  }

  std::unordered_set<std::string> output_names;
  for (const auto& column : collected) {
    if (column.deferred_variable_list) {
      continue;
    }
    if (!output_names.insert(column.name).second) {
      return PJ::unexpected("parser_arrow: duplicate output column name '" + column.name + "'");
    }
  }

  rewrite.timestamp_source_index = timestamp_source_index;
  rewrite.columns.reserve(collected.size());
  bool has_host_ingestible_data = false;
  for (auto& column : collected) {
    if (column.deferred_variable_list) {
      has_host_ingestible_data = true;
      rewrite.needs_rewrite = true;
      continue;
    }
    if (column.force_drop) {
      const auto* input_child = schemaAtPath(input_schema, column.source_path);
      rewrite.dropped_columns.push_back(
          DroppedColumn{
              column.name,
              column.dropped_format.empty() ? std::string(input_child->format) : std::move(column.dropped_format)});
      rewrite.needs_rewrite = true;
      continue;
    }
    const ArrowSchema* input_child = nullptr;
    std::string output_format;
    bool ingestible = false;
    if (column.cast == CastKind::kSyntheticTimestamp) {
      output_format = "l";
      ingestible = true;
    } else {
      input_child = schemaAtPath(input_schema, column.source_path);
      const ArrowSchema* scalar_schema = column.cast == CastKind::kListElement ? input_child->children[0] : input_child;
      const CastKind value_cast = scalarCast(column);
      if (value_cast == CastKind::kNormalizeBytes) {
        const std::string_view input_format(scalar_schema->format);
        const bool is_string = input_format == "vu" || input_format == "U";
        output_format = is_string ? "u" : "z";
        ingestible = is_string;
      } else if (castsToInt64(value_cast)) {
        output_format = "l";
        ingestible = true;
      } else {
        output_format = scalar_schema->format;
        auto host_ingestible = isHostIngestible(scalar_schema);
        if (!host_ingestible) {
          return PJ::unexpected(std::move(host_ingestible).error());
        }
        ingestible = *host_ingestible;
      }
    }

    if (!ingestible) {
      if (column.name == timestamp_column) {
        return PJ::unexpected("parser_arrow: timestamp axis cannot be dropped from the shaped Arrow stream");
      }
      rewrite.dropped_columns.push_back(DroppedColumn{column.name, std::move(output_format)});
      rewrite.needs_rewrite = true;
      continue;
    } else if (column.name != timestamp_column) {
      has_host_ingestible_data = true;
    }
    if (column.cast == CastKind::kListElement ||
        (column.cast != CastKind::kNone && column.cast != CastKind::kTimestampAxisInt64)) {
      rewrite.needs_rewrite = true;
    }
    const bool source_name_missing =
        column.source_path.size() == 1 && (input_schema->children[column.source_path[0]]->name == nullptr ||
                                           input_schema->children[column.source_path[0]]->name[0] == '\0');
    if (column.source_path.size() > 1 || source_name_missing) {
      rewrite.needs_rewrite = true;
    }
    rewrite.columns.push_back(std::move(column));
  }

  if (!has_host_ingestible_data) {
    std::string details;
    constexpr std::size_t kMaxReportedColumns = 4;
    const std::size_t reported = std::min(rewrite.dropped_columns.size(), kMaxReportedColumns);
    for (std::size_t index = 0; index < reported; ++index) {
      if (!details.empty()) {
        details += ", ";
      }
      details += rewrite.dropped_columns[index].name + ":" + rewrite.dropped_columns[index].format;
    }
    return PJ::unexpected(
        "parser_arrow: no host-ingestible columns in Arrow schema (" +
        (details.empty() ? std::string("no data columns") : details) + ")");
  }

  ArrowSchemaInit(rewrite.output_schema.out());
  int result = ArrowSchemaSetTypeStruct(rewrite.output_schema.get(), static_cast<int64_t>(rewrite.columns.size()));
  if (result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(result, nullptr));
  }
  result = copySchemaName(rewrite.output_schema.get(), input_schema->name);
  if (result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(result, nullptr));
  }
  result = copySchemaMetadata(rewrite.output_schema.get(), input_schema->metadata);
  if (result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(result, nullptr));
  }
  rewrite.output_schema.get()->flags = input_schema->flags;

  for (std::size_t output_index = 0; output_index < rewrite.columns.size(); ++output_index) {
    const auto& column = rewrite.columns[output_index];
    auto* output_child = rewrite.output_schema.get()->children[output_index];
    if (column.cast == CastKind::kSyntheticTimestamp) {
      result = ArrowSchemaSetType(output_child, NANOARROW_TYPE_INT64);
    } else {
      const auto* input_child = schemaAtPath(input_schema, column.source_path);
      if (column.cast == CastKind::kListElement) {
        input_child = input_child->children[0];
      }
      output_child->release(output_child);
      result = ArrowSchemaDeepCopy(input_child, output_child);
      const CastKind value_cast = scalarCast(column);
      if (result == NANOARROW_OK && value_cast == CastKind::kNormalizeBytes) {
        const std::string_view input_format(input_child->format);
        const ArrowType canonical_type =
            input_format == "vu" || input_format == "U" ? NANOARROW_TYPE_STRING : NANOARROW_TYPE_BINARY;
        result = ArrowSchemaSetType(output_child, canonical_type);
      } else if (result == NANOARROW_OK && castsToInt64(value_cast)) {
        result = ArrowSchemaSetType(output_child, NANOARROW_TYPE_INT64);
      }
      if (result == NANOARROW_OK && (column.nullable_struct_ancestor || column.cast == CastKind::kListElement)) {
        output_child->flags |= ARROW_FLAG_NULLABLE;
      }
    }
    if (result == NANOARROW_OK) {
      result = ArrowSchemaSetName(output_child, column.name.c_str());
    }
    if (result != NANOARROW_OK) {
      return PJ::unexpected(nanoarrowError(result, nullptr));
    }
  }

  return rewrite;
}

/// Resolve timestamp policy and build exactly one reusable internal rewrite plan.
[[nodiscard]] PJ::Expected<CompleteShapePlan> buildCompleteShapePlan(
    const ArrowSchema* schema, const ShapeOptions& options, const std::vector<VariableListWidth>& variable_widths = {},
    bool variable_widths_resolved = false) {
  if (schema == nullptr || schema->release == nullptr) {
    return PJ::unexpected("parser_arrow: invalid Arrow stream schema");
  }
  if (schema->n_children < 0 || (schema->n_children > 0 && schema->children == nullptr)) {
    return PJ::unexpected("parser_arrow: malformed Arrow stream schema children");
  }

  CompleteShapePlan complete;
  if (!options.timestamp_column.empty()) {
    if (!hasColumn(schema, options.timestamp_column)) {
      return PJ::unexpected(
          "parser_arrow: timestamp column '" + options.timestamp_column + "' is absent from the Arrow schema");
    }
    complete.shape.timestamp_column = options.timestamp_column;
  } else {
    complete.shape.timestamp_column = detectTimestampColumn(schema);
    if (complete.shape.timestamp_column.empty()) {
      complete.shape.timestamp_column = "timestamp_ns";
      complete.shape.synthesize_timestamp = true;
    }
  }

  auto rewrite = buildRewritePlan(
      schema, options, complete.shape.timestamp_column, complete.shape.synthesize_timestamp, variable_widths,
      variable_widths_resolved);
  if (!rewrite) {
    return PJ::unexpected(std::move(rewrite).error());
  }
  complete.shape.needs_rewrite = rewrite->needs_rewrite;
  complete.shape.dropped_columns = std::move(rewrite->dropped_columns);
  complete.shape.expanded_lists = rewrite->expanded_lists;
  complete.rewrite = std::move(*rewrite);
  return complete;
}

/// Resolve a mutable source array by a collected child path.
[[nodiscard]] ArrowArray* arrayAtPath(ArrowArray* root, const std::vector<int64_t>& path) {
  ArrowArray* current = root;
  for (const int64_t child_index : path) {
    current = current->children[child_index];
  }
  return current;
}

/// Return whether a struct leaf can transfer ownership without parent validity work.
[[nodiscard]] bool canMoveFlattenedLeaf(const ArrowArray* root, const std::vector<int64_t>& path) {
  if (root->offset != 0 || root->null_count != 0) {
    return false;
  }
  if (path.size() <= 1) {
    return true;
  }
  const ArrowArray* current = root;
  for (std::size_t depth = 0; depth + 1 < path.size(); ++depth) {
    current = current->children[path[depth]];
    if (current->offset != 0 || current->null_count != 0) {
      return false;
    }
  }
  return true;
}

/// Return whether one source-backed output column needs an input view for copying.
[[nodiscard]] bool needsCopy(const ArrowArray* input, const OutputColumn& column) {
  const bool cast_requires_copy = column.cast != CastKind::kNone && column.cast != CastKind::kSyntheticTimestamp &&
                                  column.cast != CastKind::kTimestampAxisInt64;
  return column.cast != CastKind::kSyntheticTimestamp &&
         (cast_requires_copy || !canMoveFlattenedLeaf(input, column.source_path));
}

/// Resolve a view and logical index while respecting offsets on every parent struct.
[[nodiscard]] const ArrowArrayView* viewAtPath(
    const ArrowArrayView* root, const std::vector<int64_t>& path, int64_t row, int64_t* leaf_row,
    bool* parent_is_null) {
  const ArrowArrayView* current = root;
  int64_t current_row = row;
  *parent_is_null = false;
  for (std::size_t depth = 0; depth < path.size(); ++depth) {
    if (ArrowArrayViewIsNull(current, current_row) != 0) {
      *parent_is_null = true;
    }
    if (current->offset > std::numeric_limits<int64_t>::max() - current_row) {
      *parent_is_null = true;
      break;
    }
    current_row += current->offset;
    current = current->children[path[depth]];
  }
  *leaf_row = current_row;
  return current;
}

/// Append one supported scalar or byte value from an ArrowArrayView.
[[nodiscard]] int appendValue(ArrowArray* output, const ArrowArrayView* input, int64_t row) {
  switch (input->storage_type) {
    case NANOARROW_TYPE_NA:
      return ArrowArrayAppendNull(output, 1);
    case NANOARROW_TYPE_BOOL:
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
      return ArrowArrayAppendInt(output, ArrowArrayViewGetIntUnsafe(input, row));
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64:
      return ArrowArrayAppendUInt(output, ArrowArrayViewGetUIntUnsafe(input, row));
    case NANOARROW_TYPE_HALF_FLOAT:
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DOUBLE:
      return ArrowArrayAppendDouble(output, ArrowArrayViewGetDoubleUnsafe(input, row));
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_FIXED_SIZE_BINARY:
    case NANOARROW_TYPE_STRING_VIEW:
    case NANOARROW_TYPE_BINARY_VIEW:
      return ArrowArrayAppendBytes(output, ArrowArrayViewGetBytesUnsafe(input, row));
    default:
      return EINVAL;
  }
}

/// Return whether appendValue can reconstruct one logical source value.
[[nodiscard]] bool supportsScalarCopy(ArrowType type) {
  switch (type) {
    case NANOARROW_TYPE_NA:
    case NANOARROW_TYPE_BOOL:
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64:
    case NANOARROW_TYPE_HALF_FLOAT:
    case NANOARROW_TYPE_FLOAT:
    case NANOARROW_TYPE_DOUBLE:
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_FIXED_SIZE_BINARY:
    case NANOARROW_TYPE_DATE32:
    case NANOARROW_TYPE_DATE64:
    case NANOARROW_TYPE_TIME32:
    case NANOARROW_TYPE_TIME64:
    case NANOARROW_TYPE_DURATION:
    case NANOARROW_TYPE_INTERVAL_MONTHS:
    case NANOARROW_TYPE_INTERVAL_DAY_TIME:
    case NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO:
    case NANOARROW_TYPE_STRING_VIEW:
    case NANOARROW_TYPE_BINARY_VIEW:
    case NANOARROW_TYPE_DECIMAL32:
    case NANOARROW_TYPE_DECIMAL64:
    case NANOARROW_TYPE_DECIMAL128:
    case NANOARROW_TYPE_DECIMAL256:
      return true;
    default:
      return false;
  }
}

/// Return the integer nanoseconds-per-tick multiplier for an Arrow timestamp cast.
[[nodiscard]] int64_t timestampMultiplier(CastKind cast) {
  switch (cast) {
    case CastKind::kTimestampSecond:
    case CastKind::kTimestampAxisSecond:
      return 1'000'000'000;
    case CastKind::kTimestampMilli:
    case CastKind::kTimestampAxisMilli:
      return 1'000'000;
    case CastKind::kTimestampMicro:
    case CastKind::kTimestampAxisMicro:
      return 1000;
    default:
      return 1;
  }
}

/// Multiply a signed tick count by a positive unit scale without overflow.
[[nodiscard]] bool checkedMultiply(int64_t value, int64_t multiplier, int64_t* output) {
  if (value > 0 && value > std::numeric_limits<int64_t>::max() / multiplier) {
    return false;
  }
  if (value < 0 && value < std::numeric_limits<int64_t>::min() / multiplier) {
    return false;
  }
  *output = value * multiplier;
  return true;
}

/// Convert floating seconds to rounded int64 nanoseconds with finite/range checks.
[[nodiscard]] bool floatingSecondsToNanoseconds(double seconds, int64_t* output) {
  if (!std::isfinite(seconds)) {
    return false;
  }
  constexpr long double kNanosecondsPerSecond = 1'000'000'000.0L;
  constexpr long double kInt64LowerBound = -0x1p63L;
  constexpr long double kInt64UpperBound = 0x1p63L;
  const long double rounded = std::round(static_cast<long double>(seconds) * kNanosecondsPerSecond);
  if (rounded < kInt64LowerBound || rounded >= kInt64UpperBound) {
    return false;
  }
  *output = static_cast<int64_t>(rounded);
  return true;
}

/// Append one copied/cast source value and preserve ordinary-column nulls.
[[nodiscard]] int appendCastedValue(
    ArrowArray* output, const ArrowArrayView* input, int64_t row, const OutputColumn& column,
    ShapingStreamState& state) {
  const CastKind cast = scalarCast(column);
  if (cast == CastKind::kNone) {
    if (column.source_type == NANOARROW_TYPE_INTERVAL_MONTHS ||
        column.source_type == NANOARROW_TYPE_INTERVAL_DAY_TIME ||
        column.source_type == NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO) {
      ArrowInterval interval{};
      ArrowIntervalInit(&interval, column.source_type);
      ArrowArrayViewGetIntervalUnsafe(input, row, &interval);
      return ArrowArrayAppendInterval(output, &interval);
    }
    if (column.source_type == NANOARROW_TYPE_DECIMAL32 || column.source_type == NANOARROW_TYPE_DECIMAL64 ||
        column.source_type == NANOARROW_TYPE_DECIMAL128 || column.source_type == NANOARROW_TYPE_DECIMAL256) {
      ArrowDecimal decimal{};
      ArrowDecimalInit(&decimal, column.decimal_bitwidth, column.decimal_precision, column.decimal_scale);
      ArrowArrayViewGetDecimalUnsafe(input, row, &decimal);
      return ArrowArrayAppendDecimal(output, &decimal);
    }
    return appendValue(output, input, row);
  }
  if (cast == CastKind::kNormalizeBytes) {
    return ArrowArrayAppendBytes(output, ArrowArrayViewGetBytesUnsafe(input, row));
  }

  int64_t converted = 0;
  switch (cast) {
    case CastKind::kTimestampSecond:
    case CastKind::kTimestampMilli:
    case CastKind::kTimestampMicro:
    case CastKind::kTimestampNano:
    case CastKind::kTimestampAxisSecond:
    case CastKind::kTimestampAxisMilli:
    case CastKind::kTimestampAxisMicro:
    case CastKind::kTimestampAxisNano:
      if (!checkedMultiply(ArrowArrayViewGetIntUnsafe(input, row), timestampMultiplier(cast), &converted)) {
        setStreamError(state, "timestamp column '" + column.name + "' overflows int64 nanoseconds");
        return ERANGE;
      }
      break;
    case CastKind::kTimestampAxisInt32:
    case CastKind::kTimestampAxisInt64:
      converted = ArrowArrayViewGetIntUnsafe(input, row);
      break;
    case CastKind::kTimestampAxisUint32:
      converted = static_cast<int64_t>(ArrowArrayViewGetUIntUnsafe(input, row));
      break;
    case CastKind::kTimestampAxisUint64: {
      const uint64_t value = ArrowArrayViewGetUIntUnsafe(input, row);
      if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        setStreamError(state, "timestamp column '" + column.name + "' exceeds INT64_MAX");
        return ERANGE;
      }
      converted = static_cast<int64_t>(value);
      break;
    }
    case CastKind::kTimestampAxisFloatSeconds:
    case CastKind::kTimestampAxisDoubleSeconds:
      if (!floatingSecondsToNanoseconds(ArrowArrayViewGetDoubleUnsafe(input, row), &converted)) {
        setStreamError(state, "timestamp column '" + column.name + "' cannot be represented as int64 nanoseconds");
        return ERANGE;
      }
      break;
    case CastKind::kNone:
    case CastKind::kSyntheticTimestamp:
    case CastKind::kNormalizeBytes:
    case CastKind::kListElement:
      return EINVAL;
  }
  return ArrowArrayAppendInt(output, converted);
}

/// Copy one source leaf while applying all ancestor and leaf validity.
[[nodiscard]] int copyColumn(
    ArrowArray* output, const ArrowArrayView* input_root, const OutputColumn& column, int64_t length,
    ShapingStreamState& state) {
  if (column.cast == CastKind::kNone && !supportsScalarCopy(column.source_type)) {
    setStreamError(state, "cannot copy complex Arrow column '" + column.name + "' while applying parent validity");
    return ENOTSUP;
  }
  int result = ArrowArrayStartAppending(output);
  if (result != NANOARROW_OK) {
    return result;
  }
  for (int64_t row = 0; row < length; ++row) {
    int64_t leaf_row = row;
    bool parent_is_null = false;
    const auto* leaf = viewAtPath(input_root, column.source_path, row, &leaf_row, &parent_is_null);
    if (column.cast == CastKind::kListElement) {
      int64_t begin = 0;
      int64_t end = 0;
      if (!parent_is_null && ArrowArrayViewIsNull(leaf, leaf_row) == 0 &&
          !listChildBounds(leaf, leaf_row, column.fixed_list_size, &begin, &end)) {
        setStreamError(state, "invalid offsets in list column '" + column.name + "'");
        return EINVAL;
      }
      if (parent_is_null || ArrowArrayViewIsNull(leaf, leaf_row) != 0 || column.element_index >= end - begin) {
        result = ArrowArrayAppendNull(output, 1);
      } else if (begin > std::numeric_limits<int64_t>::max() - column.element_index) {
        setStreamError(state, "list child offset overflows in column '" + column.name + "'");
        return ERANGE;
      } else {
        const int64_t child_row = begin + column.element_index;
        const auto* child = leaf->children[0];
        if (ArrowArrayViewIsNull(child, child_row) != 0) {
          result = ArrowArrayAppendNull(output, 1);
        } else {
          result = appendCastedValue(output, child, child_row, column, state);
        }
      }
    } else if (parent_is_null || ArrowArrayViewIsNull(leaf, leaf_row) != 0) {
      if (isTimestampAxisCast(column.cast)) {
        setStreamError(state, "timestamp column '" + column.name + "' contains a null value");
        return EINVAL;
      }
      result = ArrowArrayAppendNull(output, 1);
    } else {
      result = appendCastedValue(output, leaf, leaf_row, column, state);
    }
    if (result != NANOARROW_OK) {
      return result;
    }
  }
  return NANOARROW_OK;
}

/// Safely evaluate anchor + row * interval for a non-negative row index.
[[nodiscard]] bool syntheticTimestamp(int64_t anchor, int64_t interval, int64_t row, int64_t* timestamp) {
  if (row < 0) {
    return false;
  }
  int64_t product = 0;
  if (interval > 0) {
    if (row != 0 && interval > std::numeric_limits<int64_t>::max() / row) {
      return false;
    }
    product = interval * row;
    if (anchor > std::numeric_limits<int64_t>::max() - product) {
      return false;
    }
  } else if (interval < 0) {
    if (row != 0 && interval < std::numeric_limits<int64_t>::min() / row) {
      return false;
    }
    product = interval * row;
    if (anchor < std::numeric_limits<int64_t>::min() - product) {
      return false;
    }
  }
  *timestamp = anchor + product;
  return true;
}

/// Append this batch's section of the whole-stream synthetic timestamp sequence.
[[nodiscard]] int appendSyntheticTimestamps(ArrowArray* output, ShapingStreamState& state, int64_t length) {
  int result = ArrowArrayStartAppending(output);
  if (result != NANOARROW_OK) {
    return result;
  }
  for (int64_t row = 0; row < length; ++row) {
    int64_t timestamp = 0;
    const int64_t stream_row = state.row_offset + row;
    if (!syntheticTimestamp(state.message_timestamp_ns, state.synthetic_interval_ns, stream_row, &timestamp)) {
      setStreamError(state, "synthetic timestamp overflow at row " + std::to_string(stream_row));
      return ERANGE;
    }
    result = ArrowArrayAppendInt(output, timestamp);
    if (result != NANOARROW_OK) {
      return result;
    }
  }
  return NANOARROW_OK;
}

/// Reject null cells in the resolved top-level timestamp before moving or casting any children.
[[nodiscard]] int validateTimestampColumn(ShapingStreamState& state, const ArrowArray* input) {
  if (state.timestamp_source_index < 0) {
    return NANOARROW_OK;
  }
  ArrowArrayView input_view{};
  const std::unique_ptr<ArrowArrayView, decltype(&resetArrayView)> view_owner(&input_view, &resetArrayView);
  ArrowError error{};
  int result = ArrowArrayViewInitFromSchema(&input_view, state.input_schema.get(), &error);
  if (result == NANOARROW_OK) {
    result = ArrowArrayViewSetArray(&input_view, input, &error);
  }
  if (result != NANOARROW_OK) {
    setNanoarrowStreamError(state, result, error.message);
    return result;
  }
  const std::vector<int64_t> path = {state.timestamp_source_index};
  for (int64_t row = 0; row < input->length; ++row) {
    int64_t leaf_row = row;
    bool parent_is_null = false;
    const auto* timestamp_view = viewAtPath(&input_view, path, row, &leaf_row, &parent_is_null);
    if (parent_is_null || ArrowArrayViewIsNull(timestamp_view, leaf_row) != 0) {
      setStreamError(state, "timestamp column '" + state.timestamp_column + "' contains a null value");
      return EINVAL;
    }
  }
  return NANOARROW_OK;
}

/// Rewrite one batch according to the stream's immutable mapping.
[[nodiscard]] int rewriteBatch(ShapingStreamState& state, ArrowArray* input, ArrowArray* output) {
  ArrowError error{};
  int result = ArrowArrayInitFromSchema(output, state.output_schema.get(), &error);
  if (result != NANOARROW_OK) {
    setNanoarrowStreamError(state, result, error.message);
    return result;
  }

  ArrowArrayView input_view{};
  const std::unique_ptr<ArrowArrayView, decltype(&resetArrayView)> view_owner(&input_view, &resetArrayView);
  bool needs_input_view = false;
  for (const auto& column : state.columns) {
    if (needsCopy(input, column)) {
      needs_input_view = true;
      break;
    }
  }
  if (needs_input_view) {
    result = ArrowArrayViewInitFromSchema(&input_view, state.input_schema.get(), &error);
    if (result == NANOARROW_OK) {
      result = ArrowArrayViewSetArray(&input_view, input, &error);
    }
    if (result != NANOARROW_OK) {
      setNanoarrowStreamError(state, result, error.message);
      return result;
    }
  }

  for (std::size_t output_index = 0; output_index < state.columns.size(); ++output_index) {
    const auto& column = state.columns[output_index];
    auto* output_child = output->children[output_index];
    if (column.cast == CastKind::kSyntheticTimestamp) {
      result = appendSyntheticTimestamps(output_child, state, input->length);
    } else {
      const bool copy = needsCopy(input, column);
      if (copy) {
        result = copyColumn(output_child, &input_view, column, input->length, state);
      } else {
        output_child->release(output_child);
        ArrowArrayMove(arrayAtPath(input, column.source_path), output_child);
        result = NANOARROW_OK;
      }
    }
    if (result != NANOARROW_OK) {
      if (state.last_error.empty() && state.fixed_error == nullptr) {
        setNanoarrowStreamError(state, result, "failed to rewrite Arrow column");
      }
      return result;
    }
  }

  output->length = input->length;
  output->null_count = 0;
  result = ArrowArrayFinishBuildingDefault(output, &error);
  if (result != NANOARROW_OK) {
    setNanoarrowStreamError(state, result, error.message);
  }
  return result;
}

/// Recover the private wrapper state from an ArrowArrayStream callback.
[[nodiscard]] ShapingStreamState* streamState(ArrowArrayStream* stream) {
  return static_cast<ShapingStreamState*>(stream->private_data);
}

/// Export a fresh owned copy of the rewritten schema.
int shapingGetSchema(ArrowArrayStream* stream, ArrowSchema* output) noexcept {
  try {
    auto* state = streamState(stream);
    const int result = ArrowSchemaDeepCopy(state->output_schema.get(), output);
    if (result != NANOARROW_OK) {
      setNanoarrowStreamError(*state, result, nullptr);
    }
    return result;
  } catch (const std::exception& error) {
    setStreamError(*streamState(stream), error.what());
    return EIO;
  } catch (...) {
    setStreamError(*streamState(stream), "unknown schema rewrite error");
    return EIO;
  }
}

/// Pull and lazily rewrite one record batch from the inner stream.
int shapingGetNext(ArrowArrayStream* stream, ArrowArray* output) noexcept {
  *output = {};
  try {
    auto* state = streamState(stream);
    if (state->poisoned_error_code != NANOARROW_OK) {
      return state->poisoned_error_code;
    }
    PJ::sdk::ArrowArrayHolder input_batch;
    if (state->pending_first_batch.valid()) {
      input_batch = std::move(state->pending_first_batch);
    } else {
      if (state->input_at_eos) {
        return NANOARROW_OK;
      }
      const int input_result = state->input.get()->get_next(state->input.get(), input_batch.out());
      if (input_result != NANOARROW_OK) {
        setNanoarrowStreamError(*state, input_result, ArrowArrayStreamGetLastError(state->input.get()));
        state->poisoned_error_code = input_result;
        return input_result;
      }
    }
    if (!input_batch.valid()) {
      state->input_at_eos = true;
      return NANOARROW_OK;
    }
    if (input_batch.get()->length < 0 ||
        input_batch.get()->length > std::numeric_limits<int64_t>::max() - state->row_offset) {
      setStreamError(*state, "record batch row count overflows the stream row index");
      state->poisoned_error_code = ERANGE;
      return ERANGE;
    }

    const int validation_result = validateTimestampColumn(*state, input_batch.get());
    if (validation_result != NANOARROW_OK) {
      state->poisoned_error_code = validation_result;
      return validation_result;
    }

    if (!state->rewrite_batches) {
      state->row_offset += input_batch.get()->length;
      *output = input_batch.release();
      return NANOARROW_OK;
    }

    PJ::sdk::ArrowArrayHolder output_batch;
    const int rewrite_result = rewriteBatch(*state, input_batch.get(), output_batch.out());
    if (rewrite_result != NANOARROW_OK) {
      state->poisoned_error_code = rewrite_result;
      return rewrite_result;
    }
    state->row_offset += input_batch.get()->length;
    *output = output_batch.release();
    return NANOARROW_OK;
  } catch (const std::bad_alloc&) {
    auto* state = streamState(stream);
    setStreamError(*state, "out of memory while rewriting Arrow record batch");
    state->poisoned_error_code = ENOMEM;
    return ENOMEM;
  } catch (const std::exception& error) {
    auto* state = streamState(stream);
    setStreamError(*state, error.what());
    state->poisoned_error_code = EIO;
    return EIO;
  } catch (...) {
    auto* state = streamState(stream);
    setStreamError(*state, "unknown record-batch rewrite error");
    state->poisoned_error_code = EIO;
    return EIO;
  }
}

/// Return the most recent wrapper or inner-stream diagnostic.
const char* shapingGetLastError(ArrowArrayStream* stream) noexcept {
  const auto* state = streamState(stream);
  return state->fixed_error != nullptr ? state->fixed_error : state->last_error.c_str();
}

/// Release the inner stream, cached schemas, plan, and wrapper state.
void shapingRelease(ArrowArrayStream* stream) noexcept {
  if (stream->release == nullptr) {
    return;
  }
  delete streamState(stream);
  *stream = {};
}

}  // namespace

std::string detectTimestampColumn(const ArrowSchema* schema) {
  if (schema == nullptr || schema->children == nullptr) {
    return {};
  }
  for (int64_t index = 0; index < schema->n_children; ++index) {
    const auto* child = schema->children[index];
    if (child == nullptr || child->format == nullptr) {
      continue;
    }
    const std::string_view format(child->format);
    if (format.starts_with("ts")) {
      return child->name != nullptr ? std::string(child->name) : std::string();
    }
  }

  static constexpr std::array<std::string_view, 5> kNames = {
      "timestamp_ns", "recording_timestamp_ns", "timestamp", "time", "ts"};
  for (const std::string_view preferred : kNames) {
    if (hasColumn(schema, preferred)) {
      return std::string(preferred);
    }
  }
  return {};
}

PJ::Expected<ShapePlan> planShape(const ArrowSchema* schema, const ShapeOptions& options) {
  auto complete = buildCompleteShapePlan(schema, options);
  if (!complete) {
    return PJ::unexpected(std::move(complete).error());
  }
  return std::move(complete->shape);
}

PJ::Expected<ShapedStream> shapeStream(PJ::sdk::ArrowStreamHolder input, const ShapeOptions& options) {
  if (!input.valid()) {
    return PJ::unexpected("parser_arrow: invalid Arrow input stream");
  }

  PJ::sdk::ArrowSchemaHolder input_schema;
  const int schema_result = input.get()->get_schema(input.get(), input_schema.out());
  if (schema_result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(schema_result, ArrowArrayStreamGetLastError(input.get())));
  }
  try {
    std::vector<VariableListCandidate> variable_lists;
    for (int64_t child_index = 0; child_index < input_schema.get()->n_children; ++child_index) {
      const auto* child = input_schema.get()->children[child_index];
      std::vector<int64_t> path = {child_index};
      auto status = collectVariableListCandidates(
          child, path, outputNameComponent(child, child_index), options.flatten_structs, variable_lists);
      if (!status) {
        return PJ::unexpected(std::move(status).error());
      }
    }

    PJ::sdk::ArrowArrayHolder pending_first_batch;
    std::vector<VariableListWidth> variable_widths;
    bool input_at_eos = false;
    if (!variable_lists.empty()) {
      const int first_batch_result = input.get()->get_next(input.get(), pending_first_batch.out());
      if (first_batch_result != NANOARROW_OK) {
        return PJ::unexpected(nanoarrowError(first_batch_result, ArrowArrayStreamGetLastError(input.get())));
      }
      input_at_eos = !pending_first_batch.valid();
      auto measured = measureVariableListWidths(
          input_schema.get(), pending_first_batch.valid() ? pending_first_batch.get() : nullptr, variable_lists);
      if (!measured) {
        return PJ::unexpected(std::move(measured).error());
      }
      variable_widths = std::move(*measured);
    }

    auto complete = buildCompleteShapePlan(input_schema.get(), options, variable_widths, !variable_lists.empty());
    if (!complete) {
      return PJ::unexpected(std::move(complete).error());
    }
    auto state = std::make_unique<ShapingStreamState>();
    state->input = std::move(input);
    state->input_schema = std::move(input_schema);
    state->output_schema = std::move(complete->rewrite.output_schema);
    state->pending_first_batch = std::move(pending_first_batch);
    state->columns = std::move(complete->rewrite.columns);
    state->rewrite_batches = complete->shape.needs_rewrite;
    state->timestamp_source_index = complete->rewrite.timestamp_source_index;
    state->timestamp_column = complete->shape.timestamp_column;
    state->message_timestamp_ns = options.message_timestamp_ns;
    state->synthetic_interval_ns = options.synthetic_interval_ns;
    state->input_at_eos = input_at_eos;

    ArrowArrayStream wrapper{};
    wrapper.get_schema = &shapingGetSchema;
    wrapper.get_next = &shapingGetNext;
    wrapper.get_last_error = &shapingGetLastError;
    wrapper.release = &shapingRelease;
    wrapper.private_data = state.release();
    return ShapedStream{
        PJ::sdk::ArrowStreamHolder(wrapper), complete->shape.timestamp_column, complete->shape.synthesize_timestamp,
        std::move(complete->shape.dropped_columns), std::move(complete->shape.expanded_lists)};
  } catch (const std::bad_alloc&) {
    return PJ::unexpected("parser_arrow: out of memory while planning Arrow stream rewrite");
  } catch (const std::exception& error) {
    return PJ::unexpected(parserError(error.what()));
  } catch (...) {
    return PJ::unexpected("parser_arrow: unknown Arrow stream planning error");
  }
}

}  // namespace pj::parser_arrow
