#pragma once

#include <arrow/api.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <pj_base/time_math.hpp>
#include <string>
#include <vector>

#include "pj_arrow_helpers/arrow_helpers.hpp"

namespace PJ::ParquetHelpers {

/// Extract the file basename (without extension) from a path.
/// Handles both '/' and '\\' separators.
inline std::string basenameWithoutExt(const std::string& filepath) {
  auto slash = filepath.find_last_of("/\\");
  auto start = (slash != std::string::npos) ? slash + 1 : 0;
  auto dot = filepath.rfind('.');
  if (dot != std::string::npos && dot > start) {
    return filepath.substr(start, dot - start);
  }
  return filepath.substr(start);
}

/// First Arrow TIMESTAMP column, else the first column with a well-known time name (case-insensitive); -1 if none.
inline int findTimestampColumn(const std::shared_ptr<arrow::Schema>& schema) {
  static const std::vector<std::string> kTimestampNames = {"timestamp", "time",      "t",          "ts",   "time_stamp",
                                                           "datetime",  "date_time", "_timestamp", "_time"};

  for (int i = 0; i < schema->num_fields(); i++) {
    if (schema->field(i)->type()->id() == arrow::Type::TIMESTAMP) {
      return i;
    }
  }

  for (int i = 0; i < schema->num_fields(); i++) {
    std::string name = schema->field(i)->name();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const auto& candidate : kTimestampNames) {
      if (name == candidate) {
        return i;
      }
    }
  }

  return -1;
}

/// Scale a count of Arrow time units to nanoseconds; 0 when the result overflows int64.
inline int64_t unitToNanos(int64_t value, arrow::TimeUnit::type unit) {
  PJ::TimeUnit pj_unit = PJ::TimeUnit::kNanoseconds;
  switch (unit) {
    case arrow::TimeUnit::SECOND:
      pj_unit = PJ::TimeUnit::kSeconds;
      break;
    case arrow::TimeUnit::MILLI:
      pj_unit = PJ::TimeUnit::kMilliseconds;
      break;
    case arrow::TimeUnit::MICRO:
      pj_unit = PJ::TimeUnit::kMicroseconds;
      break;
    case arrow::TimeUnit::NANO:
      break;
  }
  return PJ::scaleToNanoseconds(value, pj_unit).value_or(0);
}

/// Nanosecond value of an Arrow array cell used as the time axis. Applies no timezone adjustment (the import path
/// does that, see adjustTimezoneNanos in parquet_source.cpp).
inline int64_t getTimestampNanos(
    const std::shared_ptr<arrow::Array>& array, int64_t index, arrow::Type::type arrow_type) {
  if (array->IsNull(index)) {
    return 0;
  }

  switch (arrow_type) {
    case arrow::Type::TIMESTAMP: {
      const auto& type = static_cast<const arrow::TimestampType&>(*array->type());
      return unitToNanos(std::static_pointer_cast<arrow::TimestampArray>(array)->Value(index), type.unit());
    }
    // TIME32/TIME64 hold a time of day, or an elapsed time (MATLAB timetables); DURATION an elapsed time.
    case arrow::Type::TIME32: {
      const auto& type = static_cast<const arrow::Time32Type&>(*array->type());
      return unitToNanos(std::static_pointer_cast<arrow::Time32Array>(array)->Value(index), type.unit());
    }
    case arrow::Type::TIME64: {
      const auto& type = static_cast<const arrow::Time64Type&>(*array->type());
      return unitToNanos(std::static_pointer_cast<arrow::Time64Array>(array)->Value(index), type.unit());
    }
    case arrow::Type::DURATION: {
      const auto& type = static_cast<const arrow::DurationType&>(*array->type());
      return unitToNanos(std::static_pointer_cast<arrow::DurationArray>(array)->Value(index), type.unit());
    }
    case arrow::Type::DATE32:  // days since the UNIX epoch
      return unitToNanos(
          int64_t{std::static_pointer_cast<arrow::Date32Array>(array)->Value(index)} * 86'400, arrow::TimeUnit::SECOND);
    case arrow::Type::DATE64:  // milliseconds since the UNIX epoch
      return unitToNanos(std::static_pointer_cast<arrow::Date64Array>(array)->Value(index), arrow::TimeUnit::MILLI);
    case arrow::Type::INT64:
      return std::static_pointer_cast<arrow::Int64Array>(array)->Value(index);
    case arrow::Type::UINT64:
      return static_cast<int64_t>(std::static_pointer_cast<arrow::UInt64Array>(array)->Value(index));
    case arrow::Type::DOUBLE: {
      double v = std::static_pointer_cast<arrow::DoubleArray>(array)->Value(index);
      return static_cast<int64_t>(v * 1e9);
    }
    case arrow::Type::FLOAT: {
      float v = std::static_pointer_cast<arrow::FloatArray>(array)->Value(index);
      return static_cast<int64_t>(static_cast<double>(v) * 1e9);
    }
    case arrow::Type::INT32:
      return static_cast<int64_t>(std::static_pointer_cast<arrow::Int32Array>(array)->Value(index));
    case arrow::Type::UINT32:
      return static_cast<int64_t>(std::static_pointer_cast<arrow::UInt32Array>(array)->Value(index));
    default:
      return 0;
  }
}

}  // namespace PJ::ParquetHelpers
