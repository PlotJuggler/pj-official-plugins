// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "arrow_ipc_message.hpp"

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/extension_type.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/writer.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <vector>

#include "arrow_timestamp.hpp"

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

// One path component: dots are separators too, so a flat `wheel.speed` and a
// nested `wheel: struct<speed>` name the same series. An empty name follows the
// consumer's rule (see EmptyNameRule).
std::string outputComponent(const std::string& name, int child_index, EmptyNameRule empty_name_rule) {
  if (name.empty()) {
    return empty_name_rule == EmptyNameRule::kIndex ? "_" + std::to_string(child_index) : std::string{};
  }
  std::string out = name;
  std::replace(out.begin(), out.end(), '.', '/');
  return out;
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
    }
    case arrow::Type::RUN_END_ENCODED:
      // ponytail: arrow::compute::RunEndDecode would materialize this, but the
      // pinned Arrow is built with compute=False, so its vector kernels
      // (run_end_decode, take) are not registered and only the cast kernels are.
      // Dropping the column keeps the siblings; flip `arrow/*:compute` in the
      // root conanfile and rewrite this arm if a producer ever emits one.
      return arrow::Status::NotImplemented(
          "field '", path, "': type ", type->ToString(),
          " needs the run_end_decode kernel, absent from this Arrow build");
    default:
      if (isIpcDecodableLeaf(type->id())) {
        return type;
      }
      return arrow::Status::NotImplemented(
          "field '", path, "': type ", type->ToString(), " cannot be decoded by parser_arrow");
  }
}

// Frame one field, salvaging what can be salvaged. The whole field first: if it
// rewrites cleanly there is nothing to project and a plain cast will do. Only a
// STRUCT gets a second chance, per child — every other container would need a
// kernel that rebuilds it around a projected child, and none exists. @p dropped
// collects the reason for each child lost this way.
arrow::Result<FieldProjection> projectField(
    const std::shared_ptr<arrow::Field>& field, int index, const std::string& path, std::vector<std::string>& dropped) {
  auto whole = ipcSafeType(field->type(), path);
  if (whole.ok()) {
    return FieldProjection{index, *whole, {}};
  }
  if (field->type()->id() != arrow::Type::STRUCT) {
    return whole.status();
  }

  FieldProjection projection{index, nullptr, {}};
  arrow::FieldVector kept;
  for (int child = 0; child < field->type()->num_fields(); ++child) {
    const auto& child_field = field->type()->field(child);
    const std::string child_path = path + "/" + outputComponent(child_field->name(), child, EmptyNameRule::kIndex);
    auto child_projection = projectField(child_field, child, child_path, dropped);
    if (!child_projection.ok()) {
      dropped.push_back(child_projection.status().message());
      continue;
    }
    kept.push_back(child_field->WithType(child_projection->type));
    projection.children.push_back(*std::move(child_projection));
  }
  if (kept.empty()) {
    return arrow::Status::NotImplemented("field '", path, "': no child can be framed");
  }
  projection.type = arrow::struct_(kept);
  return projection;
}

// Apply one projection to the ArrayData behind a source field. Working on
// ArrayData rather than Array is what makes the struct case cheap AND correct:
// a struct's children live unsliced with the parent's offset/validity applied on
// top, so swapping `type` and `child_data` on a shallow copy reassembles the
// survivors without touching a buffer or realigning a bitmap.
arrow::Result<std::shared_ptr<arrow::ArrayData>> projectData(
    const std::shared_ptr<arrow::ArrayData>& source, const FieldProjection& projection) {
  if (projection.children.empty()) {
    if (source->type->Equals(*projection.type)) {
      return source;
    }
    ARROW_ASSIGN_OR_RAISE(auto casted, arrow::compute::Cast(*arrow::MakeArray(source), projection.type));
    return casted->data();
  }
  auto data = source->Copy();
  data->type = projection.type;
  data->child_data.clear();
  data->child_data.reserve(projection.children.size());
  for (const auto& child : projection.children) {
    ARROW_ASSIGN_OR_RAISE(
        auto projected, projectData(source->child_data[static_cast<std::size_t>(child.source_index)], child));
    data->child_data.push_back(std::move(projected));
  }
  return data;
}

}  // namespace

arrow::Result<IpcSafeSchema> ipcSafeSchema(const std::shared_ptr<arrow::Schema>& schema) {
  IpcSafeSchema result;
  arrow::FieldVector fields;
  bool changed = false;
  for (int index = 0; index < schema->num_fields(); ++index) {
    const auto& field = schema->field(index);
    auto projection =
        projectField(field, index, outputComponent(field->name(), index, EmptyNameRule::kIndex), result.dropped);
    if (!projection.ok()) {
      result.dropped.push_back(projection.status().message());
      changed = true;
      continue;
    }
    changed |= projection->type != field->type();
    fields.push_back(field->WithType(projection->type));
    result.columns.push_back(*std::move(projection));
  }
  if (fields.empty()) {
    return arrow::Status::Invalid(
        fmt::format("no column parser_arrow can decode: {}", fmt::join(result.dropped, "; ")));
  }
  result.schema = changed ? std::make_shared<arrow::Schema>(fields, schema->metadata()) : schema;
  return result;
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> castToSchema(
    const arrow::RecordBatch& batch, const IpcSafeSchema& safe) {
  arrow::ArrayVector columns;
  columns.reserve(safe.columns.size());
  for (const auto& projection : safe.columns) {
    ARROW_ASSIGN_OR_RAISE(auto data, projectData(batch.column(projection.source_index)->data(), projection));
    columns.push_back(arrow::MakeArray(data));
  }
  return arrow::RecordBatch::Make(safe.schema, batch.num_rows(), std::move(columns));
}

namespace {

/// One flattened leaf: where it lives and what it is.
struct Leaf {
  std::string path;
  std::vector<int> route;
  arrow::Type::type type_id;
};

// Depth-first: STRUCT fields expand into their children, everything else is a
// leaf. @p route is the live child-index stack.
void appendLeaves(
    const arrow::FieldVector& fields, const std::string& prefix, EmptyNameRule empty_name_rule, std::vector<int>& route,
    std::vector<Leaf>& out) {
  for (int index = 0; index < static_cast<int>(fields.size()); ++index) {
    const auto& field = fields[static_cast<std::size_t>(index)];
    const std::string component = outputComponent(field->name(), index, empty_name_rule);
    std::string path = prefix.empty() ? component : prefix + "/" + component;
    route.push_back(index);
    if (field->type()->id() == arrow::Type::STRUCT) {
      appendLeaves(field->type()->fields(), path, empty_name_rule, route, out);
    } else {
      out.push_back(Leaf{std::move(path), route, field->type()->id()});
    }
    route.pop_back();
  }
}

std::vector<Leaf> collectLeaves(const arrow::Schema& schema, EmptyNameRule empty_name_rule) {
  std::vector<Leaf> leaves;
  std::vector<int> route;
  appendLeaves(schema.fields(), {}, empty_name_rule, route, leaves);
  return leaves;
}

}  // namespace

TimestampLeaf detectTimestampLeaf(const arrow::Schema& schema, EmptyNameRule empty_name_rule, PJ::TimeUnit unit) {
  const auto leaves = collectLeaves(schema, empty_name_rule);
  std::vector<PJ::sdk::TimestampCandidate> candidates;
  candidates.reserve(leaves.size());
  for (const auto& leaf : leaves) {
    candidates.push_back({leaf.path, timestampStorage(leaf.type_id)});
  }
  auto policy = PJ::sdk::kCanonicalPolicy;
  policy.unit = unit;
  if (const auto selected = PJ::sdk::detectTimestampColumn(candidates, policy)) {
    const auto& leaf = leaves[*selected];
    return {leaf.path, leaf.route};
  }
  return {};
}

std::string axisLostToFraming(const arrow::Schema& raw, const arrow::Schema& framed, EmptyNameRule empty_name_rule) {
  const std::vector<Leaf> raw_leaves = collectLeaves(raw, empty_name_rule);
  const std::vector<Leaf> framed_leaves = collectLeaves(framed, empty_name_rule);
  auto lost = [&framed_leaves](const std::string& path) {
    return std::none_of(
        framed_leaves.begin(), framed_leaves.end(), [&path](const Leaf& leaf) { return leaf.path == path; });
  };
  // The same two rules as detectTimestampLeaf, minus the plausibility gate: an
  // encoding the parser cannot decode is exactly how a real stamp gets lost.
  for (const Leaf& leaf : raw_leaves) {
    if (leaf.type_id == arrow::Type::TIMESTAMP && lost(leaf.path)) {
      return leaf.path;
    }
  }
  const Leaf* named = nullptr;
  auto priority = PJ::sdk::kCanonicalTimestampNames.size();
  for (const auto& leaf : raw_leaves) {
    const auto rank = PJ::sdk::timestampNamePriority(leaf.path);
    if (rank && *rank < priority && lost(leaf.path)) {
      named = &leaf;
      priority = *rank;
    }
  }
  if (named) {
    return named->path;
  }
  return {};
}

std::optional<std::int64_t> firstRowTimestampNs(
    const arrow::RecordBatch& batch, const std::vector<int>& route, PJ::TimeUnit unit) {
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
  return timestampNanoseconds(*array, 0, unit);
}

arrow::Result<std::shared_ptr<arrow::Buffer>> serializeIpcStream(
    const arrow::RecordBatch& batch, std::int64_t capacity_hint) {
  ARROW_ASSIGN_OR_RAISE(auto sink, arrow::io::BufferOutputStream::Create(capacity_hint > 0 ? capacity_hint : 4096));
  ARROW_ASSIGN_OR_RAISE(auto writer, arrow::ipc::MakeStreamWriter(sink, batch.schema()));
  ARROW_RETURN_NOT_OK(writer->WriteRecordBatch(batch));
  ARROW_RETURN_NOT_OK(writer->Close());
  return sink->Finish();
}

std::string parserConfigJson(std::string_view timestamp_field, std::int64_t synthetic_interval_ns, PJ::TimeUnit unit) {
  nlohmann::json config{
      {"timestamp_column", std::string(timestamp_field)},
      {"synthetic_interval_ns", synthetic_interval_ns},
      {"flatten_structs", true}};
  PJ::sdk::timestampUnitToJson(config, unit);
  return config.dump();
}

}  // namespace mosaico
