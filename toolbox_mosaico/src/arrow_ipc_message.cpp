// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "arrow_ipc_message.hpp"

#include <arrow/api.h>
#include <arrow/extension_type.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/writer.h>
#include <fmt/format.h>

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <vector>

#include "../../common/arrow_timestamp.hpp"

namespace mosaico {

namespace {

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
    std::string path = route.empty() ? component : prefix + "/" + component;
    route.push_back(index);
    auto type = timestampStorageType(field->type());
    if (type->id() == arrow::Type::STRUCT) {
      appendLeaves(type->fields(), path, empty_name_rule, route, out);
    } else {
      out.push_back(Leaf{std::move(path), route, type->id()});
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

std::optional<std::int64_t> firstRowTimestampNs(
    const arrow::RecordBatch& batch, const std::vector<int>& route, PJ::TimeUnit unit) {
  if (batch.num_rows() == 0 || route.empty() || route.front() < 0 || route.front() >= batch.num_columns()) {
    return std::nullopt;
  }
  std::shared_ptr<arrow::Array> array = batch.column(route.front());
  std::int64_t row = 0;
  for (std::size_t depth = 1; depth < route.size(); ++depth) {
    while (array->type_id() == arrow::Type::DICTIONARY || array->type_id() == arrow::Type::EXTENSION) {
      if (array->IsNull(row)) {
        return std::nullopt;
      }
      if (const auto* dictionary = dynamic_cast<const arrow::DictionaryArray*>(array.get())) {
        row = dictionary->GetValueIndex(row);
        array = dictionary->dictionary();
      } else {
        array = static_cast<const arrow::ExtensionArray&>(*array).storage();
      }
    }
    const auto* parent = dynamic_cast<const arrow::StructArray*>(array.get());
    if (parent == nullptr || parent->IsNull(row) || route[depth] < 0 || route[depth] >= parent->num_fields()) {
      return std::nullopt;
    }
    array = parent->field(route[depth]);
  }
  return timestampNanoseconds(*array, row, unit);
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> addSyntheticTimestamps(
    const arrow::RecordBatch& batch, std::int64_t anchor, std::int64_t interval, std::int64_t row_offset) {
  const auto leaves = collectLeaves(*batch.schema(), EmptyNameRule::kIndex);
  std::string name = "__mosaico_timestamp";
  while (std::any_of(leaves.begin(), leaves.end(), [&name](const Leaf& leaf) { return leaf.path == name; })) {
    name += "_";
  }
  arrow::TimestampBuilder timestamps(arrow::timestamp(arrow::TimeUnit::NANO), arrow::default_memory_pool());
  ARROW_RETURN_NOT_OK(timestamps.Reserve(batch.num_rows()));
  for (std::int64_t row = 0; row < batch.num_rows(); ++row) {
    if (row_offset < 0 || row > std::numeric_limits<std::int64_t>::max() - row_offset) {
      return arrow::Status::Invalid("synthetic row offset overflow");
    }
    const auto ns = PJ::syntheticInstant(anchor, interval, row_offset + row);
    if (!ns) {
      return arrow::Status::Invalid("synthetic timestamp overflow at row ", row_offset + row);
    }
    timestamps.UnsafeAppend(*ns);
  }
  ARROW_ASSIGN_OR_RAISE(auto array, timestamps.Finish());
  return batch.AddColumn(0, arrow::field(name, array->type(), false), array);
}

arrow::Result<std::shared_ptr<arrow::Buffer>> serializeIpcStream(
    const arrow::RecordBatch& batch, std::int64_t capacity_hint) {
  ARROW_RETURN_NOT_OK(batch.ValidateFull());
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
