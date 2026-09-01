// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "arrow_ipc_message.hpp"

#include <arrow/io/memory.h>
#include <arrow/ipc/writer.h>
#include <arrow/util/checked_cast.h>

#include <array>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace mosaico {

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

std::int64_t timestampUnitToNs(arrow::TimeUnit::type unit) {
  switch (unit) {
    case arrow::TimeUnit::SECOND:
      return 1'000'000'000LL;
    case arrow::TimeUnit::MILLI:
      return 1'000'000LL;
    case arrow::TimeUnit::MICRO:
      return 1'000LL;
    case arrow::TimeUnit::NANO:
      return 1LL;
  }
  return 1LL;
}

}  // namespace

std::optional<std::int64_t> firstRowTimestampNs(const arrow::RecordBatch& batch, std::string_view field) {
  if (batch.num_rows() == 0 || field.empty()) {
    return std::nullopt;
  }
  const auto column = batch.GetColumnByName(std::string(field));
  if (!column || column->IsNull(0)) {
    return std::nullopt;
  }
  using arrow::internal::checked_cast;
  switch (column->type_id()) {
    case arrow::Type::INT8:
      return checked_cast<const arrow::Int8Array&>(*column).Value(0);
    case arrow::Type::INT16:
      return checked_cast<const arrow::Int16Array&>(*column).Value(0);
    case arrow::Type::INT32:
      return checked_cast<const arrow::Int32Array&>(*column).Value(0);
    case arrow::Type::INT64:
      return checked_cast<const arrow::Int64Array&>(*column).Value(0);
    case arrow::Type::UINT8:
      return checked_cast<const arrow::UInt8Array&>(*column).Value(0);
    case arrow::Type::UINT16:
      return checked_cast<const arrow::UInt16Array&>(*column).Value(0);
    case arrow::Type::UINT32:
      return checked_cast<const arrow::UInt32Array&>(*column).Value(0);
    case arrow::Type::UINT64: {
      const auto value = checked_cast<const arrow::UInt64Array&>(*column).Value(0);
      if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
      }
      return static_cast<std::int64_t>(value);
    }
    case arrow::Type::FLOAT:
      return secondsToNs(checked_cast<const arrow::FloatArray&>(*column).Value(0));
    case arrow::Type::DOUBLE:
      return secondsToNs(checked_cast<const arrow::DoubleArray&>(*column).Value(0));
    case arrow::Type::TIMESTAMP: {
      const auto& array = checked_cast<const arrow::TimestampArray&>(*column);
      const auto unit = checked_cast<const arrow::TimestampType&>(*column->type()).unit();
      const std::int64_t factor = timestampUnitToNs(unit);
      const std::int64_t value = array.Value(0);
      if (value > std::numeric_limits<std::int64_t>::max() / factor ||
          value < std::numeric_limits<std::int64_t>::min() / factor) {
        return std::nullopt;
      }
      return value * factor;
    }
    default:
      return std::nullopt;
  }
}

arrow::Result<std::shared_ptr<arrow::Buffer>> serializeIpcStream(const arrow::RecordBatch& batch) {
  ARROW_ASSIGN_OR_RAISE(auto sink, arrow::io::BufferOutputStream::Create());
  ARROW_ASSIGN_OR_RAISE(auto writer, arrow::ipc::MakeStreamWriter(sink, batch.schema()));
  ARROW_RETURN_NOT_OK(writer->WriteRecordBatch(batch));
  ARROW_RETURN_NOT_OK(writer->Close());
  return sink->Finish();
}

arrow::Result<std::shared_ptr<arrow::Buffer>> serializeIpcSchema(const arrow::Schema& schema) {
  return arrow::ipc::SerializeSchema(schema);
}

std::string parserConfigJson(std::string_view timestamp_field, std::int64_t synthetic_interval_ns) {
  return nlohmann::json{
      {"timestamp_column", std::string(timestamp_field)}, {"synthetic_interval_ns", synthetic_interval_ns}}
      .dump();
}

}  // namespace mosaico
