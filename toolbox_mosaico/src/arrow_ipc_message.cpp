// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "arrow_ipc_message.hpp"

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/extension_type.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/writer.h>
#include <arrow/util/string.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <vector>

namespace mosaico {

namespace {

// Leaf types nanoarrow_ipc 0.7 decodes verbatim — the accepted arms of its
// Field type switch (ArrowIpcDecoderSetType, nanoarrow/ipc/decoder.c), minus the
// nested ones handled in ipcSafeType. Anything absent here is a decode failure,
// so it must be rewritten or refused before framing.
bool isIpcDecodableLeaf(arrow::Type::type id) {
  switch (id) {
    case arrow::Type::NA:
    case arrow::Type::BOOL:
    case arrow::Type::UINT8:
    case arrow::Type::INT8:
    case arrow::Type::UINT16:
    case arrow::Type::INT16:
    case arrow::Type::UINT32:
    case arrow::Type::INT32:
    case arrow::Type::UINT64:
    case arrow::Type::INT64:
    case arrow::Type::HALF_FLOAT:
    case arrow::Type::FLOAT:
    case arrow::Type::DOUBLE:
    case arrow::Type::DECIMAL32:
    case arrow::Type::DECIMAL64:
    case arrow::Type::DECIMAL128:
    case arrow::Type::DECIMAL256:
    case arrow::Type::STRING:
    case arrow::Type::LARGE_STRING:
    case arrow::Type::BINARY:
    case arrow::Type::LARGE_BINARY:
    case arrow::Type::FIXED_SIZE_BINARY:
    case arrow::Type::DATE32:
    case arrow::Type::DATE64:
    case arrow::Type::TIME32:
    case arrow::Type::TIME64:
    case arrow::Type::TIMESTAMP:
    case arrow::Type::DURATION:
    case arrow::Type::INTERVAL_MONTHS:
    case arrow::Type::INTERVAL_DAY_TIME:
    case arrow::Type::INTERVAL_MONTH_DAY_NANO:
      return true;
    default:
      return false;
  }
}

// Type rewrite behind ipcSafeSchema; returns the input pointer when unchanged
// and an error naming @p path when the type can neither be decoded nor cast.
arrow::Result<std::shared_ptr<arrow::DataType>> ipcSafeType(
    const std::shared_ptr<arrow::DataType>& type, const std::string& path) {
  switch (type->id()) {
    case arrow::Type::STRING_VIEW:
      return arrow::utf8();
    case arrow::Type::BINARY_VIEW:
      return arrow::binary();
    case arrow::Type::DICTIONARY:
      return ipcSafeType(std::static_pointer_cast<arrow::DictionaryType>(type)->value_type(), path);
    case arrow::Type::EXTENSION:
      // Arrow's IPC writer already emits an extension field as its STORAGE type
      // (plus ARROW:extension:* metadata), which nanoarrow decodes — main
      // imported arrow.opaque / arrow.json / uuid / geoarrow that way. Framing
      // the storage keeps that and lets a view-typed storage still be cast.
      return ipcSafeType(std::static_pointer_cast<arrow::ExtensionType>(type)->storage_type(), path);
    case arrow::Type::STRUCT: {
      arrow::FieldVector fields;
      bool changed = false;
      for (const auto& field : type->fields()) {
        ARROW_ASSIGN_OR_RAISE(auto safe, ipcSafeType(field->type(), path + "/" + field->name()));
        changed |= safe != field->type();
        fields.push_back(field->WithType(safe));
      }
      return changed ? arrow::struct_(fields) : type;
    }
    case arrow::Type::LIST:
    case arrow::Type::LARGE_LIST:
    case arrow::Type::FIXED_SIZE_LIST:
    case arrow::Type::LIST_VIEW:
    case arrow::Type::LARGE_LIST_VIEW: {
      const auto& value_field = type->field(0);
      ARROW_ASSIGN_OR_RAISE(auto safe, ipcSafeType(value_field->type(), path + "/" + value_field->name()));
      const bool is_view = type->id() == arrow::Type::LIST_VIEW || type->id() == arrow::Type::LARGE_LIST_VIEW;
      if (!is_view && safe == value_field->type()) {
        return type;
      }
      auto value = value_field->WithType(safe);
      switch (type->id()) {
        case arrow::Type::LIST:
        case arrow::Type::LIST_VIEW:
          return arrow::list(value);
        case arrow::Type::LARGE_LIST:
        case arrow::Type::LARGE_LIST_VIEW:
          return arrow::large_list(value);
        default:
          return arrow::fixed_size_list(value, std::static_pointer_cast<arrow::FixedSizeListType>(type)->list_size());
      }
    }
    case arrow::Type::MAP: {
      const auto map = std::static_pointer_cast<arrow::MapType>(type);
      ARROW_ASSIGN_OR_RAISE(auto key, ipcSafeType(map->key_type(), path + "/key"));
      ARROW_ASSIGN_OR_RAISE(auto item, ipcSafeType(map->item_type(), path + "/" + map->item_field()->name()));
      if (key == map->key_type() && item == map->item_type()) {
        return type;
      }
      return arrow::map(key, map->item_field()->WithType(item), map->keys_sorted());
    }
    case arrow::Type::SPARSE_UNION:
    case arrow::Type::DENSE_UNION: {
      // Arrow has no union-to-union cast kernel, so a union passes only when
      // every child is already decodable; one needing a rewrite is refused
      // rather than framed into a stream parser_arrow would reject.
      for (const auto& field : type->fields()) {
        ARROW_ASSIGN_OR_RAISE(auto safe, ipcSafeType(field->type(), path + "/" + field->name()));
        if (safe != field->type()) {
          return arrow::Status::NotImplemented(
              "field '", path, "/", field->name(), "': type ", field->type()->ToString(),
              " needs a rewrite Arrow cannot apply inside the enclosing union");
        }
      }
      return type;
      case arrow::Type::RUN_END_ENCODED:
        // ponytail: arrow::compute::RunEndDecode would materialize this, but the
        // pinned Arrow is built with compute=False, so its vector kernels
        // (run_end_decode, take) are not registered and only the cast kernels
        // are. Dropping the column keeps the siblings; flip `arrow/*:compute` in
        // the root conanfile and rewrite this arm if a producer ever emits one.
        return arrow::Status::NotImplemented(
            "field '", path, "': type ", type->ToString(),
            " needs the run_end_decode kernel, absent from this Arrow build");
    }
    default:
      if (isIpcDecodableLeaf(type->id())) {
        return type;
      }
      return arrow::Status::NotImplemented(
          "field '", path, "': type ", type->ToString(), " cannot be decoded by parser_arrow");
  }
}

}  // namespace

arrow::Result<IpcSafeSchema> ipcSafeSchema(const std::shared_ptr<arrow::Schema>& schema) {
  IpcSafeSchema result;
  arrow::FieldVector fields;
  bool changed = false;
  for (int index = 0; index < schema->num_fields(); ++index) {
    const auto& field = schema->field(index);
    auto safe = ipcSafeType(field->type(), field->name());
    if (!safe.ok()) {
      result.dropped.push_back(safe.status().message());
      changed = true;
      continue;
    }
    changed |= *safe != field->type();
    result.kept_columns.push_back(index);
    fields.push_back(field->WithType(*safe));
  }
  if (fields.empty()) {
    return arrow::Status::Invalid(
        "no column parser_arrow can decode: ", arrow::internal::JoinStrings(result.dropped, "; "));
  }
  result.schema = changed ? std::make_shared<arrow::Schema>(fields, schema->metadata()) : schema;
  return result;
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> castToSchema(
    const arrow::RecordBatch& batch, const IpcSafeSchema& safe) {
  arrow::ArrayVector columns;
  columns.reserve(safe.kept_columns.size());
  for (std::size_t index = 0; index < safe.kept_columns.size(); ++index) {
    std::shared_ptr<arrow::Array> column = batch.column(safe.kept_columns[index]);
    const auto& want = safe.schema->field(static_cast<int>(index))->type();
    if (!column->type()->Equals(*want)) {
      ARROW_ASSIGN_OR_RAISE(column, arrow::compute::Cast(*column, want));
    }
    columns.push_back(std::move(column));
  }
  return arrow::RecordBatch::Make(safe.schema, batch.num_rows(), std::move(columns));
}

namespace {

/// Name list scanned after the type rule, in THIS order (a later field named
/// `recording_timestamp_ns` beats an earlier `ts`).
constexpr std::array<std::string_view, 5> kTimestampNames = {
    "timestamp_ns", "recording_timestamp_ns", "timestamp", "time", "ts"};

/// One flattened leaf: where it lives and what it is.
struct Leaf {
  std::string path;
  std::vector<int> route;
  arrow::Type::type type_id;
};

// parser_arrow's childOutputName: an empty name becomes `_<child index>`, and
// dots are path separators so a flat `wheel.speed` and a nested
// `wheel: struct<speed>` name the same series.
std::string outputComponent(const std::string& name, int child_index) {
  if (name.empty()) {
    return "_" + std::to_string(child_index);
  }
  std::string out = name;
  std::replace(out.begin(), out.end(), '.', '/');
  return out;
}

// Depth-first: STRUCT fields expand into their children, everything else is a
// leaf. @p route is the live child-index stack.
void appendLeaves(
    const arrow::FieldVector& fields, const std::string& prefix, std::vector<int>& route, std::vector<Leaf>& out) {
  for (int index = 0; index < static_cast<int>(fields.size()); ++index) {
    const auto& field = fields[static_cast<std::size_t>(index)];
    const std::string component = outputComponent(field->name(), index);
    std::string path = prefix.empty() ? component : prefix + "/" + component;
    route.push_back(index);
    if (field->type()->id() == arrow::Type::STRUCT) {
      appendLeaves(field->type()->fields(), path, route, out);
    } else {
      out.push_back(Leaf{std::move(path), route, field->type()->id()});
    }
    route.pop_back();
  }
}

std::vector<Leaf> collectLeaves(const arrow::Schema& schema) {
  std::vector<Leaf> leaves;
  std::vector<int> route;
  appendLeaves(schema.fields(), {}, route, leaves);
  return leaves;
}

}  // namespace

TimestampLeaf detectTimestampLeaf(const arrow::Schema& schema) {
  const std::vector<Leaf> leaves = collectLeaves(schema);
  for (const Leaf& leaf : leaves) {
    if (leaf.type_id == arrow::Type::TIMESTAMP) {
      return {leaf.path, leaf.route};
    }
  }
  for (std::string_view candidate : kTimestampNames) {
    for (const Leaf& leaf : leaves) {
      if (leaf.path == candidate) {
        return {leaf.path, leaf.route};
      }
    }
  }
  return {};
}

namespace {

// Bit-for-bit parser_arrow's floatingSecondsToNanoseconds: the long double
// promotion before the multiply is load-bearing, not defensive — in double the
// product lands on a .5 tie often enough to round a whole nanosecond away from
// what the parser computes for the very same column.
std::optional<std::int64_t> secondsToNs(double seconds) {
  if (!std::isfinite(seconds)) {
    return std::nullopt;
  }
  constexpr long double kNanosecondsPerSecond = 1'000'000'000.0L;
  constexpr long double kInt64LowerBound = -0x1p63L;
  constexpr long double kInt64UpperBound = 0x1p63L;
  const long double rounded = std::round(static_cast<long double>(seconds) * kNanosecondsPerSecond);
  if (rounded < kInt64LowerBound || rounded >= kInt64UpperBound) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(rounded);
}

}  // namespace

std::optional<std::int64_t> firstRowTimestampNs(const arrow::RecordBatch& batch, const std::vector<int>& route) {
  if (batch.num_rows() == 0 || route.empty() || route.front() < 0 || route.front() >= batch.num_columns()) {
    return std::nullopt;
  }
  std::shared_ptr<arrow::Array> array = batch.column(route.front());
  for (std::size_t depth = 1; depth < route.size(); ++depth) {
    const auto* parent = dynamic_cast<const arrow::StructArray*>(array.get());
    if (parent == nullptr || parent->IsNull(0) || route[depth] < 0 || route[depth] >= parent->num_fields()) {
      return std::nullopt;
    }
    array = parent->field(route[depth]);
  }
  auto leaf_scalar = array->GetScalar(0);
  if (!leaf_scalar.ok() || !(*leaf_scalar)->is_valid) {
    return std::nullopt;
  }
  const std::shared_ptr<arrow::Scalar>& scalar = *leaf_scalar;
  const arrow::Type::type type_id = scalar->type->id();
  // FLOAT/DOUBLE only: parser_arrow refuses a half-float axis, so stamping from
  // one here would disagree with the parser about the very same column.
  if (type_id == arrow::Type::FLOAT || type_id == arrow::Type::DOUBLE) {
    auto seconds = arrow::compute::Cast(arrow::Datum(scalar), arrow::float64());
    return seconds.ok() ? secondsToNs(seconds->scalar_as<arrow::DoubleScalar>().value) : std::nullopt;
  }
  if (type_id == arrow::Type::TIMESTAMP) {
    auto nanos = arrow::compute::Cast(arrow::Datum(scalar), arrow::timestamp(arrow::TimeUnit::NANO));
    return nanos.ok() ? std::optional(nanos->scalar_as<arrow::TimestampScalar>().value) : std::nullopt;
  }
  if (!arrow::is_integer(type_id)) {
    return std::nullopt;
  }
  auto nanos = arrow::compute::Cast(arrow::Datum(scalar), arrow::int64());
  return nanos.ok() ? std::optional(nanos->scalar_as<arrow::Int64Scalar>().value) : std::nullopt;
}

arrow::Result<std::shared_ptr<arrow::Buffer>> serializeIpcStream(
    const arrow::RecordBatch& batch, std::int64_t capacity_hint) {
  ARROW_ASSIGN_OR_RAISE(auto sink, arrow::io::BufferOutputStream::Create(capacity_hint > 0 ? capacity_hint : 4096));
  ARROW_ASSIGN_OR_RAISE(auto writer, arrow::ipc::MakeStreamWriter(sink, batch.schema()));
  ARROW_RETURN_NOT_OK(writer->WriteRecordBatch(batch));
  ARROW_RETURN_NOT_OK(writer->Close());
  return sink->Finish();
}

std::string parserConfigJson(std::string_view timestamp_field, std::int64_t synthetic_interval_ns) {
  return nlohmann::json{
      {"timestamp_column", std::string(timestamp_field)},
      {"synthetic_interval_ns", synthetic_interval_ns},
      {"flatten_structs", true}}
      .dump();
}

}  // namespace mosaico
