#include "table_shaper.hpp"

#include <nanoarrow/nanoarrow.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pj::parser_arrow {
namespace {

/// One output column and the source path or generator used to populate it.
struct OutputColumn {
  /// Child indices from the record-batch root to the source leaf.
  std::vector<int64_t> source_path;
  /// Whether the column is generated instead of read from source_path.
  bool synthesize_timestamp = false;
  /// Whether a string/binary view must be copied to its canonical representation.
  bool normalize_view = false;
  /// Whether flattening removed a nullable struct ancestor from this leaf.
  bool nullable_struct_ancestor = false;
};

/// Schema and column mapping used by the lazy wrapping stream.
struct RewritePlan {
  PJ::sdk::ArrowSchemaHolder output_schema;
  std::vector<OutputColumn> columns;
};

/// State owned by one lazy shaping ArrowArrayStream.
struct ShapingStreamState {
  PJ::sdk::ArrowStreamHolder input;
  PJ::sdk::ArrowSchemaHolder input_schema;
  PJ::sdk::ArrowSchemaHolder output_schema;
  std::vector<OutputColumn> columns;
  int64_t message_timestamp_ns = 0;
  int64_t synthetic_interval_ns = 0;
  int64_t row_offset = 0;
  int poisoned_error_code = NANOARROW_OK;
  std::string last_error;
  const char* fixed_error = nullptr;
};

constexpr char kCallbackErrorFallback[] = "parser_arrow: unable to store Arrow stream error details";

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

/// Return whether a schema represents an Arrow string_view or binary_view.
[[nodiscard]] bool isView(const ArrowSchema* schema) {
  if (schema == nullptr || schema->format == nullptr) {
    return false;
  }
  const std::string_view format(schema->format);
  return format == "vu" || format == "vz";
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

/// Return whether flattening or view normalization changes this schema.
[[nodiscard]] bool schemaNeedsRewrite(const ArrowSchema* schema, bool flatten_structs) {
  if (schema == nullptr || schema->children == nullptr) {
    return false;
  }
  for (int64_t index = 0; index < schema->n_children; ++index) {
    const auto* child = schema->children[index];
    if (isView(child) || (flatten_structs && isStruct(child))) {
      return true;
    }
  }
  return false;
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

/// Collect depth-first output leaves and their source paths.
void collectOutputColumns(
    const ArrowSchema* schema, const std::vector<int64_t>& path, std::string name, bool flatten_structs,
    bool nullable_struct_ancestor, std::vector<std::pair<OutputColumn, std::string>>& output) {
  if (flatten_structs && isStruct(schema)) {
    const bool descendants_nullable = nullable_struct_ancestor || (schema->flags & ARROW_FLAG_NULLABLE) != 0;
    for (int64_t child_index = 0; child_index < schema->n_children; ++child_index) {
      const auto* child = schema->children[child_index];
      auto child_path = path;
      child_path.push_back(child_index);
      std::string child_name = name;
      child_name += "/";
      if (child != nullptr && child->name != nullptr) {
        child_name += child->name;
      }
      collectOutputColumns(child, child_path, std::move(child_name), flatten_structs, descendants_nullable, output);
    }
    return;
  }

  OutputColumn column;
  column.source_path = path;
  column.normalize_view = isView(schema);
  column.nullable_struct_ancestor = nullable_struct_ancestor;
  output.emplace_back(std::move(column), std::move(name));
}

/// Build the rewritten schema and output-to-input path mapping.
[[nodiscard]] PJ::Expected<RewritePlan> buildRewritePlan(
    const ArrowSchema* input_schema, const ShapeOptions& options, bool synthesize_timestamp) {
  std::vector<std::pair<OutputColumn, std::string>> collected;
  if (synthesize_timestamp) {
    OutputColumn timestamp_column;
    timestamp_column.synthesize_timestamp = true;
    collected.emplace_back(std::move(timestamp_column), "timestamp_ns");
  }

  for (int64_t child_index = 0; child_index < input_schema->n_children; ++child_index) {
    const auto* child = input_schema->children[child_index];
    std::vector<int64_t> path = {child_index};
    const std::string name = child != nullptr && child->name != nullptr ? child->name : "";
    collectOutputColumns(child, path, name, options.flatten_structs, false, collected);
  }

  RewritePlan rewrite;
  ArrowSchemaInit(rewrite.output_schema.out());
  int result = ArrowSchemaSetTypeStruct(rewrite.output_schema.get(), static_cast<int64_t>(collected.size()));
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

  rewrite.columns.reserve(collected.size());
  for (std::size_t output_index = 0; output_index < collected.size(); ++output_index) {
    auto& [column, name] = collected[output_index];
    auto* output_child = rewrite.output_schema.get()->children[output_index];
    if (column.synthesize_timestamp) {
      result = ArrowSchemaSetType(output_child, NANOARROW_TYPE_INT64);
    } else {
      const auto* input_child = schemaAtPath(input_schema, column.source_path);
      output_child->release(output_child);
      result = ArrowSchemaDeepCopy(input_child, output_child);
      if (result == NANOARROW_OK && column.normalize_view) {
        const std::string_view input_format(input_child->format);
        const ArrowType canonical_type = input_format == "vu" ? NANOARROW_TYPE_STRING : NANOARROW_TYPE_BINARY;
        result = ArrowSchemaSetType(output_child, canonical_type);
      }
      if (result == NANOARROW_OK && column.nullable_struct_ancestor) {
        output_child->flags |= ARROW_FLAG_NULLABLE;
      }
    }
    if (result == NANOARROW_OK) {
      result = ArrowSchemaSetName(output_child, name.c_str());
    }
    if (result != NANOARROW_OK) {
      return PJ::unexpected(nanoarrowError(result, nullptr));
    }
    rewrite.columns.push_back(std::move(column));
  }

  return rewrite;
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
  return !column.synthesize_timestamp && (column.normalize_view || !canMoveFlattenedLeaf(input, column.source_path));
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

/// Copy one source leaf while applying all ancestor and leaf validity.
[[nodiscard]] int copyColumn(
    ArrowArray* output, const ArrowArrayView* input_root, const std::vector<int64_t>& path, int64_t length) {
  int result = ArrowArrayStartAppending(output);
  if (result != NANOARROW_OK) {
    return result;
  }
  for (int64_t row = 0; row < length; ++row) {
    int64_t leaf_row = row;
    bool parent_is_null = false;
    const auto* leaf = viewAtPath(input_root, path, row, &leaf_row, &parent_is_null);
    if (parent_is_null || ArrowArrayViewIsNull(leaf, leaf_row) != 0) {
      result = ArrowArrayAppendNull(output, 1);
    } else {
      result = appendValue(output, leaf, leaf_row);
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
[[nodiscard]] int appendSyntheticTimestamps(ArrowArray* output, const ShapingStreamState& state, int64_t length) {
  int result = ArrowArrayStartAppending(output);
  if (result != NANOARROW_OK) {
    return result;
  }
  for (int64_t row = 0; row < length; ++row) {
    int64_t timestamp = 0;
    if (!syntheticTimestamp(
            state.message_timestamp_ns, state.synthetic_interval_ns, state.row_offset + row, &timestamp)) {
      return ERANGE;
    }
    result = ArrowArrayAppendInt(output, timestamp);
    if (result != NANOARROW_OK) {
      return result;
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
    if (column.synthesize_timestamp) {
      result = appendSyntheticTimestamps(output_child, state, input->length);
    } else {
      const bool copy = needsCopy(input, column);
      if (copy) {
        result = copyColumn(output_child, &input_view, column.source_path, input->length);
      } else {
        output_child->release(output_child);
        ArrowArrayMove(arrayAtPath(input, column.source_path), output_child);
        result = NANOARROW_OK;
      }
    }
    if (result != NANOARROW_OK) {
      setNanoarrowStreamError(state, result, "failed to rewrite Arrow column");
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
    const int input_result = state->input.get()->get_next(state->input.get(), input_batch.out());
    if (input_result != NANOARROW_OK) {
      setNanoarrowStreamError(*state, input_result, ArrowArrayStreamGetLastError(state->input.get()));
      state->poisoned_error_code = input_result;
      return input_result;
    }
    if (!input_batch.valid()) {
      return NANOARROW_OK;
    }
    if (input_batch.get()->length < 0 ||
        input_batch.get()->length > std::numeric_limits<int64_t>::max() - state->row_offset) {
      setStreamError(*state, "record batch row count overflows the stream row index");
      state->poisoned_error_code = ERANGE;
      return ERANGE;
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
  if (schema == nullptr || schema->release == nullptr) {
    return PJ::unexpected("parser_arrow: invalid Arrow stream schema");
  }
  if (schema->n_children < 0 || (schema->n_children > 0 && schema->children == nullptr)) {
    return PJ::unexpected("parser_arrow: malformed Arrow stream schema children");
  }

  ShapePlan plan;
  if (!options.timestamp_column.empty()) {
    if (!hasColumn(schema, options.timestamp_column)) {
      return PJ::unexpected(
          "parser_arrow: timestamp column '" + options.timestamp_column + "' is absent from the Arrow schema");
    }
    plan.timestamp_column = options.timestamp_column;
  } else {
    plan.timestamp_column = detectTimestampColumn(schema);
    if (plan.timestamp_column.empty()) {
      plan.timestamp_column = "timestamp_ns";
      plan.synthesize_timestamp = true;
    }
  }
  plan.needs_rewrite = plan.synthesize_timestamp || schemaNeedsRewrite(schema, options.flatten_structs);
  return plan;
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
  auto plan = planShape(input_schema.get(), options);
  if (!plan) {
    return PJ::unexpected(std::move(plan).error());
  }
  if (!plan->needs_rewrite) {
    return ShapedStream{std::move(input), plan->timestamp_column, plan->synthesize_timestamp};
  }

  try {
    auto rewrite = buildRewritePlan(input_schema.get(), options, plan->synthesize_timestamp);
    if (!rewrite) {
      return PJ::unexpected(std::move(rewrite).error());
    }
    auto state = std::make_unique<ShapingStreamState>();
    state->input = std::move(input);
    state->input_schema = std::move(input_schema);
    state->output_schema = std::move(rewrite->output_schema);
    state->columns = std::move(rewrite->columns);
    state->message_timestamp_ns = options.message_timestamp_ns;
    state->synthetic_interval_ns = options.synthetic_interval_ns;

    ArrowArrayStream wrapper{};
    wrapper.get_schema = &shapingGetSchema;
    wrapper.get_next = &shapingGetNext;
    wrapper.get_last_error = &shapingGetLastError;
    wrapper.release = &shapingRelease;
    wrapper.private_data = state.release();
    return ShapedStream{PJ::sdk::ArrowStreamHolder(wrapper), plan->timestamp_column, plan->synthesize_timestamp};
  } catch (const std::bad_alloc&) {
    return PJ::unexpected("parser_arrow: out of memory while planning Arrow stream rewrite");
  } catch (const std::exception& error) {
    return PJ::unexpected(parserError(error.what()));
  } catch (...) {
    return PJ::unexpected("parser_arrow: unknown Arrow stream planning error");
  }
}

}  // namespace pj::parser_arrow
