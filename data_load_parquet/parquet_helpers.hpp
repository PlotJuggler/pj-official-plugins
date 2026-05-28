#pragma once

#include <arrow/api.h>

#include <algorithm>
#include <cctype>
#include <memory>
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

// Heuristic: find a column that looks like a timestamp by name or type.
inline int findTimestampColumn(const std::shared_ptr<arrow::Schema>& schema) {
  static const std::vector<std::string> kTimestampNames = {"timestamp", "time",      "t",          "ts",   "time_stamp",
                                                           "datetime",  "date_time", "_timestamp", "_time"};

  // Prefer Arrow TIMESTAMP typed columns
  for (int i = 0; i < schema->num_fields(); i++) {
    if (schema->field(i)->type()->id() == arrow::Type::TIMESTAMP) {
      return i;
    }
  }

  // Fallback: match by name (case-insensitive)
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

/// Extract timestamp in nanoseconds from an Arrow array cell (for the time axis).
/// Does NOT apply timezone adjustment — callers that need the host-relative
/// nanoseconds wrap this with their own timezone post-step (the production
/// import path in parquet_source.cpp does so via adjustTimezoneNanos).
inline int64_t getTimestampNanos(
    const std::shared_ptr<arrow::Array>& array, int64_t index, arrow::Type::type arrow_type) {
  if (array->IsNull(index)) {
    return 0;
  }

  switch (arrow_type) {
    case arrow::Type::TIMESTAMP: {
      auto ts_array = std::static_pointer_cast<arrow::TimestampArray>(array);
      auto ts_type = std::static_pointer_cast<arrow::TimestampType>(ts_array->type());
      int64_t value = ts_array->Value(index);
      switch (ts_type->unit()) {
        case arrow::TimeUnit::SECOND:
          return value * 1'000'000'000LL;
        case arrow::TimeUnit::MILLI:
          return value * 1'000'000LL;
        case arrow::TimeUnit::MICRO:
          return value * 1'000LL;
        case arrow::TimeUnit::NANO:
          return value;
      }
      return value;
    }
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
