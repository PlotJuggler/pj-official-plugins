// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "arrow_ipc_message.hpp"

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/writer.h>

#include <array>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace mosaico {

namespace {

// Type rewrite behind ipcSafeSchema; returns the input pointer when unchanged.
std::shared_ptr<arrow::DataType> ipcSafeType(const std::shared_ptr<arrow::DataType>& type) {
  switch (type->id()) {
    case arrow::Type::STRING_VIEW:
      return arrow::utf8();
    case arrow::Type::BINARY_VIEW:
      return arrow::binary();
    case arrow::Type::DICTIONARY:
      return ipcSafeType(std::static_pointer_cast<arrow::DictionaryType>(type)->value_type());
    case arrow::Type::STRUCT: {
      std::vector<std::shared_ptr<arrow::Field>> fields;
      bool changed = false;
      for (const auto& field : type->fields()) {
        auto safe = ipcSafeType(field->type());
        changed |= safe != field->type();
        fields.push_back(field->WithType(safe));
      }
      return changed ? arrow::struct_(fields) : type;
    }
    case arrow::Type::LIST:
    case arrow::Type::LARGE_LIST:
    case arrow::Type::FIXED_SIZE_LIST: {
      const auto& value_field = type->field(0);
      auto safe = ipcSafeType(value_field->type());
      if (safe == value_field->type()) {
        return type;
      }
      if (type->id() == arrow::Type::LIST) {
        return arrow::list(value_field->WithType(safe));
      }
      if (type->id() == arrow::Type::LARGE_LIST) {
        return arrow::large_list(value_field->WithType(safe));
      }
      return arrow::fixed_size_list(
          value_field->WithType(safe), std::static_pointer_cast<arrow::FixedSizeListType>(type)->list_size());
    }
    // ponytail: map/union/run-end types are not rewritten; a view nested in one fails at decode with a named type — add
    // a case when a producer appears.
    default:
      return type;
  }
}

}  // namespace

std::shared_ptr<arrow::Schema> ipcSafeSchema(const std::shared_ptr<arrow::Schema>& schema) {
  std::vector<std::shared_ptr<arrow::Field>> fields;
  bool changed = false;
  for (const auto& field : schema->fields()) {
    auto safe = ipcSafeType(field->type());
    changed |= safe != field->type();
    fields.push_back(field->WithType(safe));
  }
  return changed ? std::make_shared<arrow::Schema>(fields, schema->metadata()) : schema;
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> castToSchema(
    const arrow::RecordBatch& batch, const std::shared_ptr<arrow::Schema>& target) {
  std::vector<std::shared_ptr<arrow::Array>> columns = batch.columns();
  for (int index = 0; index < batch.num_columns(); ++index) {
    const auto& want = target->field(index)->type();
    if (columns[index]->type()->Equals(*want)) {
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(auto casted, arrow::compute::Cast(*columns[index], want));
    columns[index] = std::move(casted);
  }
  return arrow::RecordBatch::Make(target, batch.num_rows(), std::move(columns));
}

std::string detectTimestampField(const arrow::Schema& schema) {
  for (const auto& field : schema.fields()) {
    if (field->type()->id() == arrow::Type::TIMESTAMP) {
      return field->name();
    }
  }
  static constexpr std::array<std::string_view, 5> kNames = {
      "timestamp_ns", "recording_timestamp_ns", "timestamp", "time", "ts"};
  for (std::string_view candidate : kNames) {
    if (schema.GetFieldByName(std::string(candidate))) {
      return std::string(candidate);
    }
  }
  return {};
}

namespace {

std::optional<std::int64_t> secondsToNs(double seconds) {
  if (!std::isfinite(seconds)) {
    return std::nullopt;
  }
  const double ns = std::round(seconds * 1e9);
  if (ns < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
      ns >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(ns);
}

}  // namespace

std::optional<std::int64_t> firstRowTimestampNs(const arrow::RecordBatch& batch, int index) {
  if (batch.num_rows() == 0 || index < 0 || index >= batch.num_columns()) {
    return std::nullopt;
  }
  auto scalar = batch.column(index)->GetScalar(0);
  if (!scalar.ok() || !(*scalar)->is_valid) {
    return std::nullopt;
  }
  const arrow::Type::type type_id = (*scalar)->type->id();
  if (arrow::is_floating(type_id)) {
    auto seconds = arrow::compute::Cast(arrow::Datum(*scalar), arrow::float64());
    return seconds.ok() ? secondsToNs(seconds->scalar_as<arrow::DoubleScalar>().value) : std::nullopt;
  }
  if (type_id == arrow::Type::TIMESTAMP) {
    auto nanos = arrow::compute::Cast(arrow::Datum(*scalar), arrow::timestamp(arrow::TimeUnit::NANO));
    return nanos.ok() ? std::optional(nanos->scalar_as<arrow::TimestampScalar>().value) : std::nullopt;
  }
  if (!arrow::is_integer(type_id)) {
    return std::nullopt;
  }
  auto nanos = arrow::compute::Cast(arrow::Datum(*scalar), arrow::int64());
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

std::string parserConfigJson(std::string_view timestamp_field) {
  return nlohmann::json{
      {"timestamp_column", std::string(timestamp_field)}, {"synthetic_interval_ns", kSyntheticIntervalNs}}
      .dump();
}

}  // namespace mosaico
