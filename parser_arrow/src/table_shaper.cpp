#include "table_shaper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <nanoarrow/nanoarrow.hpp>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "arrow_error.hpp"
#include "type_table.hpp"

namespace pj::parser_arrow {
namespace {

/// Planning state for a scalar output or an ingestible variable list awaiting its first-batch width.
enum class ColumnStatus { kReady, kVariableList };

/// One output column and the source path or cast used to populate it.
struct OutputColumn {
  std::vector<int64_t> source_path;
  ColumnSource source = ColumnSource::kLeaf;
  ValueCast cast = ValueCast::kNone;
  PJ::TimeUnit unit = PJ::TimeUnit::kNanoseconds;
  CopyKind copy = CopyKind::kUnsupported;
  bool is_timestamp_axis = false;
  int64_t element_index = -1;
  int32_t fixed_list_size = 0;
  bool nullable_struct_ancestor = false;
  ColumnStatus status = ColumnStatus::kReady;
  std::size_t source_order = 0;
  std::string name;
  std::string list_source_name;
  ArrowType source_type = NANOARROW_TYPE_UNINITIALIZED;
  int32_t decimal_bitwidth = 0;
  int32_t decimal_precision = 0;
  int32_t decimal_scale = 0;
};

/// One dropped source column paired with its depth-first collection order.
struct OrderedDroppedColumn {
  std::size_t source_order = 0;
  DroppedColumn column;
};

/// Final host-compatible columns, schema, diagnostics, and optional first-batch peek.
struct CompatibilityResult {
  PJ::sdk::ArrowSchemaHolder output_schema;
  PJ::sdk::ArrowArrayHolder pending_first_batch;
  std::vector<OutputColumn> columns;
  std::vector<DroppedColumn> dropped_columns;
  bool input_at_eos = false;
};

/// State owned by one lazy shaping ArrowArrayStream.
struct ShapingStreamState {
  PJ::sdk::ArrowStreamHolder input;
  PJ::sdk::ArrowSchemaHolder input_schema;
  PJ::sdk::ArrowSchemaHolder output_schema;
  PJ::sdk::ArrowArrayHolder pending_first_batch;
  std::vector<OutputColumn> columns;
  std::shared_ptr<RuntimeStats> runtime;
  int64_t message_timestamp_ns = 0;
  int64_t synthetic_interval_ns = 0;
  int64_t row_offset = 0;
  bool input_at_eos = false;
  int poisoned_error_code = NANOARROW_OK;
  std::string last_error;
  const char* fixed_error = nullptr;
};

constexpr char kCallbackErrorFallback[] = "parser_arrow: unable to store Arrow stream error details";

/// Parse one schema node into a validated view.
[[nodiscard]] PJ::Expected<ArrowSchemaView> schemaView(const ArrowSchema* schema) {
  if (schema == nullptr || schema->format == nullptr) {
    return PJ::unexpected(parserError("malformed Arrow child schema"));
  }
  ArrowSchemaView view{};
  ArrowError error{};
  const int result = ArrowSchemaViewInit(&view, schema, &error);
  if (result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(result, error.message));
  }
  return view;
}

/// Initialize and bind a reusable array view.
[[nodiscard]] int bindArrayView(
    ArrowArrayView* view, const ArrowSchema* schema, const ArrowArray* array, ArrowError* error) {
  int result = ArrowArrayViewInitFromSchema(view, schema, error);
  if (result == NANOARROW_OK) {
    result = ArrowArrayViewSetArray(view, array, error);
  }
  return result;
}

/// Store a callback diagnostic without allowing allocation failure to cross the C ABI.
void setStreamError(ShapingStreamState& state, std::string_view message) noexcept {
  try {
    state.last_error = parserError(message);
    state.fixed_error = nullptr;
  } catch (...) {
    state.fixed_error = kCallbackErrorFallback;
  }
}

/// Couple every errno-style return with the callback diagnostic required to interpret it.
[[nodiscard]] int fail(ShapingStreamState& state, int code, std::string_view message) noexcept {
  setStreamError(state, message);
  return code;
}

/// Store a nanoarrow callback diagnostic with an errno fallback and no exception escape.
void setNanoarrowStreamError(ShapingStreamState& state, int result, const char* message) noexcept {
  try {
    const std::string diagnostic = nanoarrowError(result, message);
    state.last_error = diagnostic;
    state.fixed_error = nullptr;
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

/// Build the stable slash-separated output name for one nullable or empty child name. Dots inside a field name are
/// path separators too, so a flat `wheel.speed` and a nested `wheel: struct<speed>` produce the same series name.
[[nodiscard]] std::string childOutputName(std::string_view parent_name, const ArrowSchema* child, int64_t child_index) {
  std::string component = child != nullptr && child->name != nullptr && child->name[0] != '\0'
                              ? std::string(child->name)
                              : "_" + std::to_string(child_index);
  std::replace(component.begin(), component.end(), '.', '/');
  return parent_name.empty() ? component : std::string(parent_name) + "/" + component;
}

/// Copy a nullable C string into a nanoarrow-owned schema field.
[[nodiscard]] int copySchemaName(ArrowSchema* output, const char* name) {
  return name == nullptr ? NANOARROW_OK : ArrowSchemaSetName(output, name);
}

/// Copy nullable C Data schema metadata into nanoarrow-owned storage.
[[nodiscard]] int copySchemaMetadata(ArrowSchema* output, const char* metadata) {
  return metadata == nullptr ? NANOARROW_OK : ArrowSchemaSetMetadata(output, metadata);
}

/// Resolve a child schema by a path collected from the same root.
[[nodiscard]] const ArrowSchema* schemaAtPath(const ArrowSchema* root, const std::vector<int64_t>& path) {
  const ArrowSchema* current = root;
  for (const int64_t child_index : path) {
    current = current->children[child_index];
  }
  return current;
}

/// Resolve a mutable source array by a collected child path.
[[nodiscard]] ArrowArray* arrayAtPath(ArrowArray* root, const std::vector<int64_t>& path) {
  ArrowArray* current = root;
  for (const int64_t child_index : path) {
    current = current->children[child_index];
  }
  return current;
}

/// Return whether an Arrow integer type is unsigned, so its ticks must be read through the uint64 accessor.
[[nodiscard]] bool isUnsignedInteger(ArrowType type) {
  return typeRow(type).is_unsigned_integer;
}

/// Return whether a logical type uses Arrow list offsets or a schema-declared fixed width.
[[nodiscard]] bool isListType(ArrowType type) {
  return typeRow(type).is_list_type;
}

/// Translate Arrow's native unit without changing the timestamp's interpretation.
[[nodiscard]] PJ::TimeUnit timestampUnit(ArrowTimeUnit unit) {
  switch (unit) {
    case NANOARROW_TIME_UNIT_SECOND:
      return PJ::TimeUnit::kSeconds;
    case NANOARROW_TIME_UNIT_MILLI:
      return PJ::TimeUnit::kMilliseconds;
    case NANOARROW_TIME_UNIT_MICRO:
      return PJ::TimeUnit::kMicroseconds;
    case NANOARROW_TIME_UNIT_NANO:
      return PJ::TimeUnit::kNanoseconds;
  }
  return PJ::TimeUnit::kNanoseconds;
}

/// Parse a source exactly once and populate its scalar reconstruction and intrinsic axis cast. `column.name` must
/// already hold the flattened output name: axis rejections quote it.
PJ::Status configureSource(const ArrowSchema* schema, bool is_timestamp_axis, OutputColumn& column) {
  auto view = schemaView(schema);
  if (!view) {
    return PJ::unexpected(std::move(view).error());
  }

  const TypeRow row = typeRow(view->type);
  if (is_timestamp_axis) {
    if (!row.axis_cast.has_value()) {
      return PJ::unexpected(
          parserError("timestamp column '" + column.name + "' has unsupported type '" + schema->format + "'"));
    }
    column.cast = *row.axis_cast;
  } else {
    column.cast = ValueCast::kNone;
  }

  if (view->type == NANOARROW_TYPE_TIMESTAMP) {
    column.unit = timestampUnit(view->time_unit);
  }
  column.source_type = view->type;
  column.copy = row.copy;
  column.decimal_bitwidth = view->decimal_bitwidth;
  column.decimal_precision = view->decimal_precision;
  column.decimal_scale = view->decimal_scale;
  return PJ::okStatus();
}

/// Collect depth-first leaves with only intrinsic list casts; the timestamp axis is tagged afterwards.
PJ::Status collectOutputColumns(
    const ArrowSchema* schema, const std::vector<int64_t>& path, std::string name, bool flatten_structs,
    bool nullable_struct_ancestor, std::vector<OutputColumn>& output) {
  if (schema == nullptr) {
    return PJ::unexpected(parserError("malformed null Arrow child schema"));
  }

  if (flatten_structs && isStruct(schema)) {
    const bool descendants_nullable = nullable_struct_ancestor || (schema->flags & ARROW_FLAG_NULLABLE) != 0;
    for (int64_t child_index = 0; child_index < schema->n_children; ++child_index) {
      const auto* child = schema->children[child_index];
      auto child_path = path;
      child_path.push_back(child_index);
      auto status = collectOutputColumns(
          child, child_path, childOutputName(name, child, child_index), flatten_structs, descendants_nullable, output);
      if (!status) {
        return status;
      }
    }
    return PJ::okStatus();
  }

  auto view = schemaView(schema);
  if (!view) {
    return PJ::unexpected(std::move(view).error());
  }
  if (isListType(view->type)) {
    if (schema->n_children != 1 || schema->children == nullptr || schema->children[0] == nullptr) {
      return PJ::unexpected(parserError("malformed Arrow list schema"));
    }
    OutputColumn column;
    column.source_path = path;
    column.source = ColumnSource::kListElement;
    column.fixed_list_size = view->type == NANOARROW_TYPE_FIXED_SIZE_LIST ? view->fixed_size : int32_t{0};
    column.status = view->type == NANOARROW_TYPE_FIXED_SIZE_LIST ? ColumnStatus::kReady : ColumnStatus::kVariableList;
    column.nullable_struct_ancestor = nullable_struct_ancestor;
    column.name = std::move(name);
    column.source_order = output.size();
    auto status = configureSource(schema->children[0], false, column);
    if (!status) {
      return status;
    }
    output.push_back(std::move(column));
    return PJ::okStatus();
  }

  OutputColumn column;
  column.source_path = path;
  column.nullable_struct_ancestor = nullable_struct_ancestor;
  column.name = std::move(name);
  column.source_order = output.size();
  auto status = configureSource(schema, false, column);
  if (!status) {
    return status;
  }
  output.push_back(std::move(column));
  return PJ::okStatus();
}

/// Collect every flattened leaf of a stream schema in depth-first order.
PJ::Status collectLeaves(const ArrowSchema* schema, bool flatten_structs, std::vector<OutputColumn>& output) {
  for (int64_t child_index = 0; child_index < schema->n_children; ++child_index) {
    const auto* child = schema->children[child_index];
    auto status = collectOutputColumns(
        child, std::vector<int64_t>{child_index}, childOutputName({}, child, child_index), flatten_structs, false,
        output);
    if (!status) {
      return status;
    }
  }
  return PJ::okStatus();
}

/// Return the index of the leaf named `name`, or `leaves.size()` when no leaf matches.
[[nodiscard]] std::size_t findLeaf(const std::vector<OutputColumn>& leaves, std::string_view name) {
  std::size_t index = 0;
  while (index < leaves.size() && leaves[index].name != name) {
    ++index;
  }
  return index;
}

/// Surface the SDK's warning when an explicitly selected axis has limited range or precision.
void appendExplicitAxisWarning(const OutputColumn& column, std::vector<ShapeWarning>& warnings) {
  const auto limitation = PJ::sdk::explicitOnlyWarning(typeRow(column.source_type).timestamp_storage, column.unit);
  if (!limitation.empty()) {
    warnings.push_back(
        ShapeWarning{
            "parser_arrow.narrow_timestamp_axis",
            "explicit timestamp column '" + column.name + "': " + std::string(limitation)});
  }
}

/// Both the producer and parser use the SDK's native-type and canonical-name policy.
[[nodiscard]] std::size_t detectTimestampLeaf(const std::vector<OutputColumn>& leaves, PJ::TimeUnit unit) {
  std::vector<PJ::sdk::TimestampCandidate> candidates;
  candidates.reserve(leaves.size());
  for (const auto& leaf : leaves) {
    candidates.push_back({leaf.name, typeRow(leaf.source_type).timestamp_storage, leaf.source != ColumnSource::kLeaf});
  }
  auto policy = PJ::sdk::kCanonicalPolicy;
  policy.unit = unit;
  return PJ::sdk::detectTimestampColumn(candidates, policy).value_or(leaves.size());
}

/// Return whether the current PlotJuggler host imports a final output schema.
[[nodiscard]] PJ::Expected<bool> isHostIngestible(const ArrowSchema* schema) {
  auto view = schemaView(schema);
  if (!view) {
    return PJ::unexpected(std::move(view).error());
  }
  return typeRow(view->type).host_ingestible;
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

/// Return the final schema format and whether the host accepts it.
[[nodiscard]] PJ::Expected<std::pair<std::string, bool>> hostFormat(
    const ArrowSchema* scalar_schema, const OutputColumn& column) {
  if (column.cast == ValueCast::kNormalizeBytes) {
    const std::string_view input_format(scalar_schema->format);
    const bool is_string = input_format == "vu" || input_format == "U";
    return std::pair<std::string, bool>{is_string ? "u" : "z", is_string};
  }
  if (column.cast == ValueCast::kScaleTimestampTicks || column.cast == ValueCast::kWidenToInt64 ||
      column.cast == ValueCast::kFloatSecondsToNanoseconds) {
    return std::pair<std::string, bool>{"l", true};
  }
  auto ingestible = isHostIngestible(scalar_schema);
  if (!ingestible) {
    return PJ::unexpected(std::move(ingestible).error());
  }
  return std::pair<std::string, bool>{scalar_schema->format, *ingestible};
}

/// Append one expanded list's selected columns or report the source list once when it is empty or skipped.
void finalizeList(
    const OutputColumn& source, int64_t real_width, const ArrowSchema* source_schema,
    const PJ::sdk::ArrayLimit& array_limit, std::vector<OutputColumn>& output,
    std::vector<OrderedDroppedColumn>& dropped) {
  int64_t output_width = real_width;
  const bool oversized = array_limit.max_size > 0 && output_width > static_cast<int64_t>(array_limit.max_size);
  if (oversized && !array_limit.clamp()) {
    dropped.push_back(OrderedDroppedColumn{source.source_order, DroppedColumn{source.name, source_schema->format}});
    return;
  }
  if (oversized) {
    output_width = static_cast<int64_t>(array_limit.max_size);
  }
  if (output_width == 0) {
    dropped.push_back(
        OrderedDroppedColumn{
            source.source_order, DroppedColumn{source.name, std::string(source_schema->format) + "(empty)"}});
    return;
  }
  for (int64_t element_index = 0; element_index < output_width; ++element_index) {
    OutputColumn column = source;
    column.status = ColumnStatus::kReady;
    column.element_index = element_index;
    column.list_source_name = source.name;
    column.name = source.name + "[" + std::to_string(element_index) + "]";
    output.push_back(std::move(column));
  }
}

/// Build a host-compatible schema for the finalized output mapping.
PJ::Status buildOutputSchema(
    const ArrowSchema* input_schema, const std::vector<OutputColumn>& columns, ArrowSchema* output_schema) {
  ArrowSchemaInit(output_schema);
  int result = ArrowSchemaSetTypeStruct(output_schema, static_cast<int64_t>(columns.size()));
  if (result == NANOARROW_OK) {
    result = copySchemaName(output_schema, input_schema->name);
  }
  if (result == NANOARROW_OK) {
    result = copySchemaMetadata(output_schema, input_schema->metadata);
  }
  if (result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(result, nullptr));
  }
  output_schema->flags = input_schema->flags;

  for (std::size_t output_index = 0; output_index < columns.size(); ++output_index) {
    const auto& column = columns[output_index];
    auto* output_child = output_schema->children[output_index];
    if (column.source == ColumnSource::kSynthesizedTimestamp) {
      result = ArrowSchemaSetType(output_child, NANOARROW_TYPE_INT64);
    } else {
      const auto* source = schemaAtPath(input_schema, column.source_path);
      if (column.source == ColumnSource::kListElement) {
        source = source->children[0];
      }
      output_child->release(output_child);
      result = ArrowSchemaDeepCopy(source, output_child);
      if (result == NANOARROW_OK && column.cast == ValueCast::kNormalizeBytes) {
        const std::string_view input_format(source->format);
        result = ArrowSchemaSetType(
            output_child, input_format == "vu" || input_format == "U" ? NANOARROW_TYPE_STRING : NANOARROW_TYPE_BINARY);
      } else if (
          result == NANOARROW_OK &&
          (column.cast == ValueCast::kScaleTimestampTicks || column.cast == ValueCast::kWidenToInt64 ||
           column.cast == ValueCast::kFloatSecondsToNanoseconds)) {
        result = ArrowSchemaSetType(output_child, NANOARROW_TYPE_INT64);
      }
      if (result == NANOARROW_OK && (column.nullable_struct_ancestor || column.source == ColumnSource::kListElement)) {
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
  return PJ::okStatus();
}

/// Apply every compatibility workaround required by pj_datastore's current arrow_import implementation.
///
/// Everything in here exists only because pj_datastore's arrow_import maps only int/uint/float/double/bool/utf8,
/// drops TIMESTAMP, mis-reads LARGE_STRING offsets, and turns an empty timestamp name into row indices; delete when
/// the host is fixed.
[[nodiscard]] PJ::Expected<CompatibilityResult> applyHostCompatibility(
    std::vector<OutputColumn> collected, const ArrowSchema* input_schema, PJ::sdk::ArrowStreamHolder& input,
    bool synthesize_timestamp, const ShapeOptions& options) {
  CompatibilityResult result;
  std::vector<OutputColumn> compatible;
  std::vector<OrderedDroppedColumn> dropped;
  compatible.reserve(collected.size());
  bool has_variable_list = false;

  for (auto& column : collected) {
    const auto* source_schema = schemaAtPath(input_schema, column.source_path);
    if (column.source == ColumnSource::kListElement) {
      const auto* element_schema = source_schema->children[0];
      if (column.source_type == NANOARROW_TYPE_TIMESTAMP) {
        column.cast = ValueCast::kScaleTimestampTicks;
      } else if (needsByteNormalization(element_schema)) {
        column.cast = ValueCast::kNormalizeBytes;
      }
      auto format = hostFormat(element_schema, column);
      if (!format) {
        return PJ::unexpected(std::move(format).error());
      }
      if (!format->second) {
        dropped.push_back(OrderedDroppedColumn{column.source_order, DroppedColumn{column.name, source_schema->format}});
        continue;
      }
      if (column.status == ColumnStatus::kVariableList) {
        has_variable_list = true;
        compatible.push_back(std::move(column));
      } else {
        finalizeList(column, column.fixed_list_size, source_schema, options.array_limit, compatible, dropped);
      }
      continue;
    }

    if (!column.is_timestamp_axis && column.source_type == NANOARROW_TYPE_TIMESTAMP) {
      column.cast = ValueCast::kScaleTimestampTicks;
    } else if (needsByteNormalization(source_schema)) {
      column.cast = ValueCast::kNormalizeBytes;
    }
    auto format = hostFormat(source_schema, column);
    if (!format) {
      return PJ::unexpected(std::move(format).error());
    }
    if (!format->second) {
      if (column.is_timestamp_axis) {
        return PJ::unexpected(parserError("timestamp axis cannot be dropped from the shaped Arrow stream"));
      }
      dropped.push_back(
          OrderedDroppedColumn{column.source_order, DroppedColumn{column.name, std::move(format->first)}});
      continue;
    }
    compatible.push_back(std::move(column));
  }

  nanoarrow::UniqueArrayView first_batch_view;
  if (has_variable_list) {
    const int first_batch_result = input.get()->get_next(input.get(), result.pending_first_batch.out());
    if (first_batch_result != NANOARROW_OK) {
      return PJ::unexpected(nanoarrowError(first_batch_result, ArrowArrayStreamGetLastError(input.get())));
    }
    result.input_at_eos = !result.pending_first_batch.valid();
    if (result.pending_first_batch.valid()) {
      ArrowError error{};
      const int view_result =
          bindArrayView(first_batch_view.get(), input_schema, result.pending_first_batch.get(), &error);
      if (view_result != NANOARROW_OK) {
        return PJ::unexpected(nanoarrowError(view_result, error.message));
      }
    }
  }

  result.columns.reserve(compatible.size() + (synthesize_timestamp ? 1U : 0U));
  if (synthesize_timestamp) {
    OutputColumn synthetic;
    synthetic.source = ColumnSource::kSynthesizedTimestamp;
    synthetic.is_timestamp_axis = true;
    synthetic.name = "timestamp_ns";
    result.columns.push_back(std::move(synthetic));
  }
  for (const auto& column : compatible) {
    if (column.status != ColumnStatus::kVariableList) {
      result.columns.push_back(column);
      continue;
    }
    int64_t width = 0;
    if (result.pending_first_batch.valid()) {
      for (int64_t row = 0; row < result.pending_first_batch.get()->length; ++row) {
        int64_t list_row = row;
        bool parent_is_null = false;
        const auto* list = viewAtPath(first_batch_view.get(), column.source_path, row, &list_row, &parent_is_null);
        if (parent_is_null || ArrowArrayViewIsNull(list, list_row) != 0) {
          continue;
        }
        int64_t begin = 0;
        int64_t end = 0;
        if (!listChildBounds(list, list_row, 0, &begin, &end)) {
          return PJ::unexpected(parserError("invalid offsets in list column '" + column.name + "'"));
        }
        width = std::max(width, end - begin);
      }
    }
    finalizeList(
        column, width, schemaAtPath(input_schema, column.source_path), options.array_limit, result.columns, dropped);
  }

  struct OrderedName {
    std::size_t source_order;
    std::string_view name;
  };
  std::vector<OrderedName> names;
  names.reserve(result.columns.size() + dropped.size());
  for (const auto& column : result.columns) {
    names.push_back(OrderedName{column.source_order, column.name});
  }
  for (const auto& entry : dropped) {
    names.push_back(OrderedName{entry.source_order, entry.column.name});
  }
  std::stable_sort(names.begin(), names.end(), [](const OrderedName& left, const OrderedName& right) {
    return left.source_order < right.source_order;
  });
  std::unordered_set<std::string_view> output_names;
  for (const auto& entry : names) {
    if (!output_names.insert(entry.name).second) {
      return PJ::unexpected(parserError("duplicate output column name '" + std::string(entry.name) + "'"));
    }
  }

  bool has_host_ingestible_data = false;
  for (const auto& column : result.columns) {
    if (!column.is_timestamp_axis) {
      has_host_ingestible_data = true;
    }
  }
  std::stable_sort(
      dropped.begin(), dropped.end(), [](const OrderedDroppedColumn& left, const OrderedDroppedColumn& right) {
        return left.source_order < right.source_order;
      });
  result.dropped_columns.reserve(dropped.size());
  for (auto& entry : dropped) {
    result.dropped_columns.push_back(std::move(entry.column));
  }
  if (!has_host_ingestible_data) {
    const std::string details = formatDroppedColumns(result.dropped_columns, kMaxDroppedColumnsListed);
    return PJ::unexpected(parserError(
        "no host-ingestible columns in Arrow schema (" + (details.empty() ? std::string("no data columns") : details) +
        ")"));
  }

  auto schema_status = buildOutputSchema(input_schema, result.columns, result.output_schema.out());
  if (!schema_status) {
    return PJ::unexpected(std::move(schema_status).error());
  }
  return result;
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
  if (column.source == ColumnSource::kSynthesizedTimestamp) {
    return false;
  }
  const bool cast_requires_copy = column.cast != ValueCast::kNone && !(column.cast == ValueCast::kWidenToInt64 &&
                                                                       column.source_type == NANOARROW_TYPE_INT64 &&
                                                                       column.unit == PJ::TimeUnit::kNanoseconds);
  return cast_requires_copy || !canMoveFlattenedLeaf(input, column.source_path);
}

/// Append one supported scalar or byte value from an ArrowArrayView.
[[nodiscard]] int appendValue(
    ArrowArray* output, const ArrowArrayView* input, int64_t row, const OutputColumn& column,
    ShapingStreamState& state) {
  switch (column.copy) {
    case CopyKind::kUnsupported:
      return fail(state, EINVAL, "Arrow column '" + column.name + "' has no scalar append path");
    case CopyKind::kNull:
      return ArrowArrayAppendNull(output, 1);
    case CopyKind::kInt:
      return ArrowArrayAppendInt(output, ArrowArrayViewGetIntUnsafe(input, row));
    case CopyKind::kUInt:
      return ArrowArrayAppendUInt(output, ArrowArrayViewGetUIntUnsafe(input, row));
    case CopyKind::kDouble:
      return ArrowArrayAppendDouble(output, ArrowArrayViewGetDoubleUnsafe(input, row));
    case CopyKind::kBytes:
      return ArrowArrayAppendBytes(output, ArrowArrayViewGetBytesUnsafe(input, row));
    case CopyKind::kInterval: {
      ArrowInterval interval{};
      ArrowIntervalInit(&interval, column.source_type);
      ArrowArrayViewGetIntervalUnsafe(input, row, &interval);
      return ArrowArrayAppendInterval(output, &interval);
    }
    case CopyKind::kDecimal: {
      ArrowDecimal decimal{};
      ArrowDecimalInit(&decimal, column.decimal_bitwidth, column.decimal_precision, column.decimal_scale);
      ArrowArrayViewGetDecimalUnsafe(input, row, &decimal);
      return ArrowArrayAppendDecimal(output, &decimal);
    }
  }
  return fail(state, EINVAL, "Arrow column '" + column.name + "' has invalid scalar copy metadata");
}

/// Return whether appendValue can reconstruct one logical source value.
[[nodiscard]] bool supportsScalarCopy(ArrowType type) {
  return typeRow(type).copy != CopyKind::kUnsupported;
}

/// Append one copied/cast source value and preserve ordinary-column nulls.
[[nodiscard]] int appendCastedValue(
    ArrowArray* output, const ArrowArrayView* input, int64_t row, const OutputColumn& column,
    ShapingStreamState& state) {
  std::optional<int64_t> converted;
  switch (column.cast) {
    case ValueCast::kNone:
      return appendValue(output, input, row, column, state);
    case ValueCast::kNormalizeBytes:
      return ArrowArrayAppendBytes(output, ArrowArrayViewGetBytesUnsafe(input, row));
    case ValueCast::kScaleTimestampTicks: {
      converted = PJ::scaleToNanoseconds(ArrowArrayViewGetIntUnsafe(input, row), column.unit);
      if (!converted) {
        return fail(state, ERANGE, "timestamp column '" + column.name + "' overflows int64 nanoseconds");
      }
      break;
    }
    case ValueCast::kWidenToInt64:
      if (!column.is_timestamp_axis) {
        return fail(state, EINVAL, "non-axis column '" + column.name + "' requested timestamp integer widening");
      }
      if (isUnsignedInteger(column.source_type)) {
        converted = PJ::toSignedTicks(ArrowArrayViewGetUIntUnsafe(input, row));
        if (!converted) {
          return fail(state, ERANGE, "timestamp column '" + column.name + "' exceeds INT64_MAX");
        }
      } else {
        converted = ArrowArrayViewGetIntUnsafe(input, row);
      }
      converted = PJ::scaleToNanoseconds(*converted, column.unit);
      if (!converted) {
        return fail(state, ERANGE, "timestamp column '" + column.name + "' overflows int64 nanoseconds");
      }
      break;
    case ValueCast::kFloatSecondsToNanoseconds: {
      if (!column.is_timestamp_axis) {
        return fail(state, EINVAL, "non-axis column '" + column.name + "' requested timestamp float conversion");
      }
      const double seconds = ArrowArrayViewGetDoubleUnsafe(input, row);
      if (column.source_type == NANOARROW_TYPE_FLOAT && state.runtime != nullptr &&
          !state.runtime->float_axis_magnitude_exceeded && std::abs(seconds) >= 0x1p23) {
        state.runtime->float_axis_column = column.name;
        state.runtime->float_axis_magnitude_exceeded = true;
      }
      converted = PJ::secondsToNanoseconds(seconds);
      if (!converted) {
        return fail(state, ERANGE, "timestamp column '" + column.name + "' cannot be represented as int64 nanoseconds");
      }
      break;
    }
  }
  return ArrowArrayAppendInt(output, *converted);
}

/// Copy one source leaf while applying all ancestor and leaf validity.
[[nodiscard]] int copyColumn(
    ArrowArray* output, const ArrowArrayView* input_root, const OutputColumn& column, int64_t length,
    ShapingStreamState& state) {
  if (column.cast == ValueCast::kNone && !supportsScalarCopy(column.source_type)) {
    return fail(
        state, ENOTSUP, "cannot copy complex Arrow column '" + column.name + "' while applying parent validity");
  }
  int result = ArrowArrayStartAppending(output);
  if (result != NANOARROW_OK) {
    return result;
  }
  for (int64_t row = 0; row < length; ++row) {
    int64_t leaf_row = row;
    bool parent_is_null = false;
    const auto* leaf = viewAtPath(input_root, column.source_path, row, &leaf_row, &parent_is_null);
    if (parent_is_null || ArrowArrayViewIsNull(leaf, leaf_row) != 0) {
      if (column.is_timestamp_axis) {
        return fail(state, EINVAL, "timestamp column '" + column.name + "' contains a null value");
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

/// Copy all expanded columns from one source list, resolving its path and row bounds once per row.
[[nodiscard]] int copyListColumns(
    ArrowArray* output, std::size_t first, std::size_t last, const ArrowArrayView* input_root,
    const std::vector<OutputColumn>& columns, int64_t length, ShapingStreamState& state) {
  struct ListRowState {
    const ArrowArrayView* child = nullptr;
    int64_t begin = 0;
    int64_t end = 0;
    bool is_null = true;
  };
  std::vector<ListRowState> rows(static_cast<std::size_t>(length));

  const auto& first_column = columns[first];
  const int64_t selected_width = static_cast<int64_t>(last - first);
  for (std::size_t index = first; index < last; ++index) {
    int result = ArrowArrayStartAppending(output->children[index]);
    if (result != NANOARROW_OK) {
      return result;
    }
    const auto& column = columns[index];
    ArrowArray* output_child = output->children[index];
    for (int64_t row = 0; row < length; ++row) {
      auto& list_row_state = rows[static_cast<std::size_t>(row)];
      if (index == first) {
        int64_t list_row = row;
        bool parent_is_null = false;
        const auto* list = viewAtPath(input_root, first_column.source_path, row, &list_row, &parent_is_null);
        list_row_state.is_null = parent_is_null || ArrowArrayViewIsNull(list, list_row) != 0;
        list_row_state.child = list->children[0];
        if (!list_row_state.is_null &&
            !listChildBounds(
                list, list_row, first_column.fixed_list_size, &list_row_state.begin, &list_row_state.end)) {
          return fail(state, EINVAL, "invalid offsets in list column '" + first_column.name + "'");
        }
        if (!list_row_state.is_null && list_row_state.end - list_row_state.begin > selected_width &&
            state.runtime != nullptr) {
          if (state.runtime->first_truncated_column.empty()) {
            state.runtime->first_truncated_column = first_column.list_source_name;
          }
          if (state.runtime->rows_truncated < std::numeric_limits<int64_t>::max()) {
            ++state.runtime->rows_truncated;
          }
        }
      }

      if (list_row_state.is_null || column.element_index >= list_row_state.end - list_row_state.begin) {
        result = ArrowArrayAppendNull(output_child, 1);
      } else if (list_row_state.begin > std::numeric_limits<int64_t>::max() - column.element_index) {
        return fail(state, ERANGE, "list child offset overflows in column '" + column.name + "'");
      } else {
        const int64_t child_row = list_row_state.begin + column.element_index;
        result = ArrowArrayViewIsNull(list_row_state.child, child_row) != 0
                     ? ArrowArrayAppendNull(output_child, 1)
                     : appendCastedValue(output_child, list_row_state.child, child_row, column, state);
      }
      if (result != NANOARROW_OK) {
        return result;
      }
    }
  }
  return NANOARROW_OK;
}

/// Append this batch's section of the whole-stream synthetic timestamp sequence.
[[nodiscard]] int appendSyntheticTimestamps(ArrowArray* output, ShapingStreamState& state, int64_t length) {
  int result = ArrowArrayStartAppending(output);
  if (result != NANOARROW_OK) {
    return result;
  }
  for (int64_t row = 0; row < length; ++row) {
    const int64_t stream_row = state.row_offset + row;
    const auto timestamp = PJ::syntheticInstant(state.message_timestamp_ns, state.synthetic_interval_ns, stream_row);
    if (!timestamp) {
      return fail(state, ERANGE, "synthetic timestamp overflow at row " + std::to_string(stream_row));
    }
    result = ArrowArrayAppendInt(output, *timestamp);
    if (result != NANOARROW_OK) {
      return result;
    }
  }
  return NANOARROW_OK;
}

/// Rewrite one batch according to the stream's immutable mapping and one shared input view.
[[nodiscard]] int rewriteBatch(
    ShapingStreamState& state, ArrowArray* input, const ArrowArrayView* input_view, ArrowArray* output) {
  for (const auto& column : state.columns) {
    if (!column.is_timestamp_axis || column.source == ColumnSource::kSynthesizedTimestamp) {
      continue;
    }
    const ArrowArray* source = arrayAtPath(input, column.source_path);
    if (input->null_count == 0 && source->null_count == 0) {
      break;
    }
    for (int64_t row = 0; row < input->length; ++row) {
      int64_t leaf_row = row;
      bool parent_is_null = false;
      const auto* leaf = viewAtPath(input_view, column.source_path, row, &leaf_row, &parent_is_null);
      if (parent_is_null || ArrowArrayViewIsNull(leaf, leaf_row) != 0) {
        return fail(state, EINVAL, "timestamp column '" + column.name + "' contains a null value");
      }
    }
    break;
  }

  ArrowError error{};
  int result = ArrowArrayInitFromSchema(output, state.output_schema.get(), &error);
  if (result != NANOARROW_OK) {
    setNanoarrowStreamError(state, result, error.message);
    return result;
  }

  std::size_t output_index = 0;
  while (output_index < state.columns.size()) {
    const auto& column = state.columns[output_index];
    if (column.source == ColumnSource::kListElement) {
      std::size_t group_end = output_index + 1;
      while (group_end < state.columns.size() && state.columns[group_end].source == ColumnSource::kListElement &&
             state.columns[group_end].source_path == column.source_path) {
        ++group_end;
      }
      result = copyListColumns(output, output_index, group_end, input_view, state.columns, input->length, state);
      output_index = group_end;
    } else if (column.source == ColumnSource::kSynthesizedTimestamp) {
      result = appendSyntheticTimestamps(output->children[output_index], state, input->length);
      ++output_index;
    } else {
      ArrowArray* source = arrayAtPath(input, column.source_path);
      const bool copy = needsCopy(input, column) || (column.is_timestamp_axis && source->null_count != 0);
      if (copy) {
        result = copyColumn(output->children[output_index], input_view, column, input->length, state);
      } else {
        output->children[output_index]->release(output->children[output_index]);
        ArrowArrayMove(source, output->children[output_index]);
        result = NANOARROW_OK;
      }
      ++output_index;
    }
    if (result != NANOARROW_OK) {
      if (state.last_error.empty() && state.fixed_error == nullptr) {
        setNanoarrowStreamError(state, result, ("failed to rewrite Arrow column '" + column.name + "'").c_str());
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
    if (state->poisoned_error_code == NANOARROW_OK) {
      state->last_error.clear();
      state->fixed_error = nullptr;
    }
    const int result = ArrowSchemaDeepCopy(state->output_schema.get(), output);
    if (result != NANOARROW_OK) {
      setNanoarrowStreamError(*state, result, nullptr);
    }
    return result;
  } catch (const std::exception& error) {
    return fail(*streamState(stream), EIO, error.what());
  } catch (...) {
    return fail(*streamState(stream), EIO, "unknown schema rewrite error");
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
    state->last_error.clear();
    state->fixed_error = nullptr;
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
      state->poisoned_error_code = fail(*state, ERANGE, "record batch row count overflows the stream row index");
      return state->poisoned_error_code;
    }

    nanoarrow::UniqueArrayView input_view;
    ArrowError error{};
    const int view_result = bindArrayView(input_view.get(), state->input_schema.get(), input_batch.get(), &error);
    if (view_result != NANOARROW_OK) {
      setNanoarrowStreamError(*state, view_result, error.message);
      state->poisoned_error_code = view_result;
      return view_result;
    }

    PJ::sdk::ArrowArrayHolder output_batch;
    const int rewrite_result = rewriteBatch(*state, input_batch.get(), input_view.get(), output_batch.out());
    if (rewrite_result != NANOARROW_OK) {
      state->poisoned_error_code = rewrite_result;
      return rewrite_result;
    }
    state->row_offset += input_batch.get()->length;
    *output = output_batch.release();
    return NANOARROW_OK;
  } catch (const std::bad_alloc&) {
    auto* state = streamState(stream);
    state->poisoned_error_code = fail(*state, ENOMEM, "out of memory while rewriting Arrow record batch");
    return state->poisoned_error_code;
  } catch (const std::exception& error) {
    auto* state = streamState(stream);
    state->poisoned_error_code = fail(*state, EIO, error.what());
    return state->poisoned_error_code;
  } catch (...) {
    auto* state = streamState(stream);
    state->poisoned_error_code = fail(*state, EIO, "unknown record-batch rewrite error");
    return state->poisoned_error_code;
  }
}

/// Return the most recent wrapper or inner-stream diagnostic.
const char* shapingGetLastError(ArrowArrayStream* stream) noexcept {
  const auto* state = streamState(stream);
  return state->fixed_error != nullptr ? state->fixed_error : state->last_error.c_str();
}

/// Release the inner stream, cached schemas, mapping, and wrapper state.
void shapingRelease(ArrowArrayStream* stream) noexcept {
  if (stream->release == nullptr) {
    return;
  }
  delete streamState(stream);
  *stream = {};
}

}  // namespace

namespace test {

/// Let the contract test follow the production predicate instead of re-reading the table it is validating.
[[nodiscard]] bool supportsScalarCopyForTesting(ArrowType type) noexcept {
  return supportsScalarCopy(type);
}

/// Exercise production scalar-copy dispatch without exposing its shaping state to tests.
[[nodiscard]] int appendCastedValueForTesting(
    ArrowArray* output, const ArrowArrayView* input, int64_t row, const ArrowSchema* logical_schema) {
  ShapingStreamState state;
  OutputColumn column;
  column.name = "copy_contract";
  auto status = configureSource(logical_schema, false, column);
  if (!status) {
    return fail(state, EINVAL, status.error());
  }
  if (!supportsScalarCopy(column.source_type)) {
    return fail(state, ENOTSUP, "logical Arrow type is not copyable");
  }
  return appendCastedValue(output, input, row, column, state);
}

}  // namespace test

std::string formatDroppedColumns(const std::vector<DroppedColumn>& columns, std::size_t max_listed) {
  std::string details;
  const std::size_t listed = std::min(columns.size(), max_listed);
  for (std::size_t index = 0; index < listed; ++index) {
    if (!details.empty()) {
      details += ", ";
    }
    details += columns[index].name + ":" + columns[index].format;
  }
  if (listed < columns.size()) {
    if (!details.empty()) {
      details += ", ";
    }
    details += "…";
  }
  return details;
}

PJ::Expected<ShapedStream> shapeStream(PJ::sdk::ArrowStreamHolder input, const ShapeOptions& options) {
  if (!input.valid()) {
    return PJ::unexpected(parserError("invalid Arrow input stream"));
  }

  PJ::sdk::ArrowSchemaHolder input_schema;
  const int schema_result = input.get()->get_schema(input.get(), input_schema.out());
  if (schema_result != NANOARROW_OK) {
    return PJ::unexpected(nanoarrowError(schema_result, ArrowArrayStreamGetLastError(input.get())));
  }
  try {
    const ArrowSchema* schema = input_schema.get();
    if (schema == nullptr || schema->release == nullptr) {
      return PJ::unexpected(parserError("invalid Arrow stream schema"));
    }
    if (schema->n_children < 0 || (schema->n_children > 0 && schema->children == nullptr)) {
      return PJ::unexpected(parserError("malformed Arrow stream schema children"));
    }

    std::vector<OutputColumn> collected;
    if (auto status = collectLeaves(schema, options.flatten_structs, collected); !status) {
      return PJ::unexpected(std::move(status).error());
    }

    std::vector<ShapeWarning> warnings;
    std::size_t axis = collected.size();
    if (options.timestamp_column.empty()) {
      axis = detectTimestampLeaf(collected, options.timestamp_unit);
    } else {
      // The configured name goes through the same dot normalization as the leaves, so a field literally named
      // `sensor.time` stays selectable by its own spelling as well as by `sensor/time`.
      std::string wanted = options.timestamp_column;
      std::replace(wanted.begin(), wanted.end(), '.', '/');
      axis = findLeaf(collected, wanted);
      if (axis == collected.size()) {
        return PJ::unexpected(
            parserError("timestamp column '" + options.timestamp_column + "' is absent from the Arrow schema"));
      }
    }

    const bool synthesize = axis == collected.size();
    std::string timestamp_name = "timestamp_ns";
    if (!synthesize) {
      collected[axis].is_timestamp_axis = true;
      auto status = configureSource(schemaAtPath(schema, collected[axis].source_path), true, collected[axis]);
      if (!status) {
        return PJ::unexpected(std::move(status).error());
      }
      if (collected[axis].source_type != NANOARROW_TYPE_TIMESTAMP) {
        collected[axis].unit = options.timestamp_unit;
      }
      if (!options.timestamp_column.empty()) {
        appendExplicitAxisWarning(collected[axis], warnings);
      }
      timestamp_name = collected[axis].name;
    } else {
      warnings.push_back(
          ShapeWarning{
              "parser_arrow.synthetic_timestamp",
              "no plausible timestamp axis found; using synthetic timestamp_ns with synthetic_interval_ns=" +
                  std::to_string(options.synthetic_interval_ns)});
    }

    auto compatible = applyHostCompatibility(std::move(collected), schema, input, synthesize, options);
    if (!compatible) {
      return PJ::unexpected(std::move(compatible).error());
    }

    auto runtime = std::make_shared<RuntimeStats>();
    auto state = std::make_unique<ShapingStreamState>();
    state->input = std::move(input);
    state->input_schema = std::move(input_schema);
    state->output_schema = std::move(compatible->output_schema);
    state->pending_first_batch = std::move(compatible->pending_first_batch);
    state->columns = std::move(compatible->columns);
    state->runtime = runtime;
    state->message_timestamp_ns = options.message_timestamp_ns;
    state->synthetic_interval_ns = options.synthetic_interval_ns;
    state->input_at_eos = compatible->input_at_eos;

    ArrowArrayStream wrapper{};
    wrapper.get_schema = &shapingGetSchema;
    wrapper.get_next = &shapingGetNext;
    wrapper.get_last_error = &shapingGetLastError;
    wrapper.release = &shapingRelease;
    wrapper.private_data = state.release();
    return ShapedStream{
        .stream = PJ::sdk::ArrowStreamHolder(wrapper),
        .timestamp_column = std::move(timestamp_name),
        .dropped_columns = std::move(compatible->dropped_columns),
        .warnings = std::move(warnings),
        .synthetic_axis = synthesize,
        .runtime = std::move(runtime),
    };
  } catch (const std::bad_alloc&) {
    return PJ::unexpected(parserError("out of memory while planning Arrow stream rewrite"));
  } catch (const std::exception& error) {
    return PJ::unexpected(parserError(error.what()));
  } catch (...) {
    return PJ::unexpected(parserError("unknown Arrow stream planning error"));
  }
}

}  // namespace pj::parser_arrow
