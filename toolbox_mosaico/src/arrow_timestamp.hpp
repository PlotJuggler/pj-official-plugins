// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <arrow/array.h>
#include <arrow/array/array_primitive.h>

#include <pj_plugins/sdk/timestamp_policy.hpp>

namespace mosaico {

/// Map Arrow storage to the shared axis-selection policy.
[[nodiscard]] inline PJ::sdk::TimestampStorage timestampStorage(arrow::Type::type type) {
  using Storage = PJ::sdk::TimestampStorage;
  switch (type) {
    case arrow::Type::TIMESTAMP:
      return Storage::kNativeTimestamp;
    case arrow::Type::INT64:
      return Storage::kInt64;
    case arrow::Type::UINT64:
      return Storage::kUInt64;
    case arrow::Type::INT32:
      return Storage::kInt32;
    case arrow::Type::UINT32:
      return Storage::kUInt32;
    case arrow::Type::DOUBLE:
      return Storage::kFloat64;
    case arrow::Type::FLOAT:
      return Storage::kFloat32;
    case arrow::Type::INT8:
    case arrow::Type::UINT8:
    case arrow::Type::INT16:
    case arrow::Type::UINT16:
      return Storage::kNarrowInt;
    default:
      return Storage::kOther;
  }
}

[[nodiscard]] inline PJ::TimeUnit timestampUnit(arrow::TimeUnit::type unit) {
  switch (unit) {
    case arrow::TimeUnit::SECOND:
      return PJ::TimeUnit::kSeconds;
    case arrow::TimeUnit::MILLI:
      return PJ::TimeUnit::kMilliseconds;
    case arrow::TimeUnit::MICRO:
      return PJ::TimeUnit::kMicroseconds;
    case arrow::TimeUnit::NANO:
      return PJ::TimeUnit::kNanoseconds;
  }
  return PJ::TimeUnit::kNanoseconds;
}

/// Read the same checked timestamp for the IPC envelope and canonical objects.
/// Floating values carry seconds; native timestamps carry their Arrow unit.
[[nodiscard]] inline std::optional<std::int64_t> timestampNanoseconds(
    const arrow::Array& array, std::int64_t row, PJ::TimeUnit unit = PJ::TimeUnit::kNanoseconds) {
  if (row < 0 || row >= array.length() || array.IsNull(row)) {
    return std::nullopt;
  }
  std::optional<std::int64_t> ticks;
  switch (array.type_id()) {
    case arrow::Type::FLOAT:
      return PJ::secondsToNanoseconds(static_cast<const arrow::FloatArray&>(array).Value(row));
    case arrow::Type::DOUBLE:
      return PJ::secondsToNanoseconds(static_cast<const arrow::DoubleArray&>(array).Value(row));
    case arrow::Type::TIMESTAMP:
      unit = timestampUnit(static_cast<const arrow::TimestampType&>(*array.type()).unit());
      ticks = static_cast<const arrow::TimestampArray&>(array).Value(row);
      break;
    case arrow::Type::INT64:
      ticks = static_cast<const arrow::Int64Array&>(array).Value(row);
      break;
    case arrow::Type::UINT64:
      ticks = PJ::toSignedTicks(static_cast<const arrow::UInt64Array&>(array).Value(row));
      break;
    case arrow::Type::INT32:
      ticks = static_cast<const arrow::Int32Array&>(array).Value(row);
      break;
    case arrow::Type::UINT32:
      ticks = static_cast<const arrow::UInt32Array&>(array).Value(row);
      break;
    case arrow::Type::INT16:
      ticks = static_cast<const arrow::Int16Array&>(array).Value(row);
      break;
    case arrow::Type::UINT16:
      ticks = static_cast<const arrow::UInt16Array&>(array).Value(row);
      break;
    case arrow::Type::INT8:
      ticks = static_cast<const arrow::Int8Array&>(array).Value(row);
      break;
    case arrow::Type::UINT8:
      ticks = static_cast<const arrow::UInt8Array&>(array).Value(row);
      break;
    default:
      return std::nullopt;
  }
  return ticks ? PJ::scaleToNanoseconds(*ticks, unit) : std::nullopt;
}

}  // namespace mosaico
