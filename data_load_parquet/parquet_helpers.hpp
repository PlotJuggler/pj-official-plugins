#pragma once

#include <pj_base/sdk/data_source_patterns.hpp>

#include <arrow/api.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

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

inline bool isSupportedArrowType(arrow::Type::type t) {
  return t == arrow::Type::BOOL || t == arrow::Type::INT8 || t == arrow::Type::INT16 ||
         t == arrow::Type::INT32 || t == arrow::Type::INT64 || t == arrow::Type::UINT8 ||
         t == arrow::Type::UINT16 || t == arrow::Type::UINT32 ||
         t == arrow::Type::UINT64 || t == arrow::Type::FLOAT ||
         t == arrow::Type::DOUBLE || t == arrow::Type::TIMESTAMP ||
         t == arrow::Type::STRING || t == arrow::Type::LARGE_STRING;
}

/// Map Arrow type to PJ::PrimitiveType for field pre-registration.
inline PJ::PrimitiveType arrowTypeToPrimitive(arrow::Type::type t) {
  switch (t) {
    case arrow::Type::BOOL: return PJ::PrimitiveType::kBool;
    case arrow::Type::INT8: return PJ::PrimitiveType::kInt8;
    case arrow::Type::INT16: return PJ::PrimitiveType::kInt16;
    case arrow::Type::INT32: return PJ::PrimitiveType::kInt32;
    case arrow::Type::INT64: return PJ::PrimitiveType::kInt64;
    case arrow::Type::UINT8: return PJ::PrimitiveType::kUint8;
    case arrow::Type::UINT16: return PJ::PrimitiveType::kUint16;
    case arrow::Type::UINT32: return PJ::PrimitiveType::kUint32;
    case arrow::Type::UINT64: return PJ::PrimitiveType::kUint64;
    case arrow::Type::FLOAT: return PJ::PrimitiveType::kFloat32;
    case arrow::Type::DOUBLE: return PJ::PrimitiveType::kFloat64;
    case arrow::Type::TIMESTAMP: return PJ::PrimitiveType::kInt64;  // nanoseconds
    case arrow::Type::STRING: return PJ::PrimitiveType::kString;
    case arrow::Type::LARGE_STRING: return PJ::PrimitiveType::kString;
    default: return PJ::PrimitiveType::kFloat64;
  }
}

// Heuristic: find a column that looks like a timestamp by name or type.
inline int findTimestampColumn(const std::shared_ptr<arrow::Schema>& schema) {
  static const std::vector<std::string> kTimestampNames = {
      "timestamp", "time", "t", "ts", "time_stamp", "datetime", "date_time",
      "_timestamp", "_time"};

  // Prefer Arrow TIMESTAMP typed columns
  for (int i = 0; i < schema->num_fields(); i++) {
    if (schema->field(i)->type()->id() == arrow::Type::TIMESTAMP) {
      return i;
    }
  }

  // Fallback: match by name (case-insensitive)
  for (int i = 0; i < schema->num_fields(); i++) {
    std::string name = schema->field(i)->name();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (const auto& candidate : kTimestampNames) {
      if (name == candidate) return i;
    }
  }

  return -1;
}

/// Extract a native-typed ValueRef from an Arrow array cell.
/// Returns NullValue for nulls and unsupported types.
inline PJ::sdk::ValueRef getArrowValueRef(const std::shared_ptr<arrow::Array>& array,
                                          int64_t index, arrow::Type::type arrow_type) {
  if (array->IsNull(index)) return PJ::NullValue{};

  switch (arrow_type) {
    case arrow::Type::BOOL:
      return std::static_pointer_cast<arrow::BooleanArray>(array)->Value(index);
    case arrow::Type::INT8:
      return std::static_pointer_cast<arrow::Int8Array>(array)->Value(index);
    case arrow::Type::INT16:
      return std::static_pointer_cast<arrow::Int16Array>(array)->Value(index);
    case arrow::Type::INT32:
      return std::static_pointer_cast<arrow::Int32Array>(array)->Value(index);
    case arrow::Type::INT64:
      return std::static_pointer_cast<arrow::Int64Array>(array)->Value(index);
    case arrow::Type::UINT8:
      return std::static_pointer_cast<arrow::UInt8Array>(array)->Value(index);
    case arrow::Type::UINT16:
      return std::static_pointer_cast<arrow::UInt16Array>(array)->Value(index);
    case arrow::Type::UINT32:
      return std::static_pointer_cast<arrow::UInt32Array>(array)->Value(index);
    case arrow::Type::UINT64:
      return std::static_pointer_cast<arrow::UInt64Array>(array)->Value(index);
    case arrow::Type::FLOAT:
      return std::static_pointer_cast<arrow::FloatArray>(array)->Value(index);
    case arrow::Type::DOUBLE:
      return std::static_pointer_cast<arrow::DoubleArray>(array)->Value(index);
    case arrow::Type::TIMESTAMP: {
      auto ts_array = std::static_pointer_cast<arrow::TimestampArray>(array);
      auto ts_type = std::static_pointer_cast<arrow::TimestampType>(ts_array->type());
      int64_t value = ts_array->Value(index);
      // Convert to nanoseconds
      switch (ts_type->unit()) {
        case arrow::TimeUnit::SECOND: value *= 1000000000LL; break;
        case arrow::TimeUnit::MILLI: value *= 1000000LL; break;
        case arrow::TimeUnit::MICRO: value *= 1000LL; break;
        case arrow::TimeUnit::NANO: break;
      }
      return value;
    }
    case arrow::Type::STRING: {
      auto str_array = std::static_pointer_cast<arrow::StringArray>(array);
      auto sv = str_array->GetView(index);
      return std::string_view(sv.data(), sv.size());
    }
    case arrow::Type::LARGE_STRING: {
      auto str_array = std::static_pointer_cast<arrow::LargeStringArray>(array);
      auto sv = str_array->GetView(index);
      return std::string_view(sv.data(), sv.size());
    }
    default:
      return PJ::NullValue{};
  }
}

/// Extract timestamp in nanoseconds from an Arrow array cell (for the time axis).
inline int64_t getTimestampNanos(const std::shared_ptr<arrow::Array>& array,
                                  int64_t index, arrow::Type::type arrow_type) {
  if (array->IsNull(index)) return 0;

  switch (arrow_type) {
    case arrow::Type::TIMESTAMP: {
      auto ts_array = std::static_pointer_cast<arrow::TimestampArray>(array);
      auto ts_type = std::static_pointer_cast<arrow::TimestampType>(ts_array->type());
      int64_t value = ts_array->Value(index);
      switch (ts_type->unit()) {
        case arrow::TimeUnit::SECOND: return value * 1000000000LL;
        case arrow::TimeUnit::MILLI: return value * 1000000LL;
        case arrow::TimeUnit::MICRO: return value * 1000LL;
        case arrow::TimeUnit::NANO: return value;
      }
      return value;
    }
    case arrow::Type::INT64:
      return std::static_pointer_cast<arrow::Int64Array>(array)->Value(index);
    case arrow::Type::UINT64:
      return static_cast<int64_t>(
          std::static_pointer_cast<arrow::UInt64Array>(array)->Value(index));
    case arrow::Type::DOUBLE: {
      double v = std::static_pointer_cast<arrow::DoubleArray>(array)->Value(index);
      return static_cast<int64_t>(v * 1e9);
    }
    case arrow::Type::FLOAT: {
      float v = std::static_pointer_cast<arrow::FloatArray>(array)->Value(index);
      return static_cast<int64_t>(static_cast<double>(v) * 1e9);
    }
    case arrow::Type::INT32:
      return static_cast<int64_t>(
          std::static_pointer_cast<arrow::Int32Array>(array)->Value(index));
    case arrow::Type::UINT32:
      return static_cast<int64_t>(
          std::static_pointer_cast<arrow::UInt32Array>(array)->Value(index));
    default:
      return 0;
  }
}

}  // namespace PJ::ParquetHelpers
