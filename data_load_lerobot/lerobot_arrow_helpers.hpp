// Arrow cell-extraction helpers, mirroring data_load_parquet's ParquetHelpers
// (each plugin in this repo is self-contained), plus LeRobot-specific support
// for flattening float list<>/fixed_size_list<> vector columns.
#pragma once

#include <pj_base/sdk/data_source_patterns.hpp>

#include <arrow/api.h>

#include <cstdint>
#include <memory>

namespace lerobot::arrow_helpers {

/// Scalar Arrow types we ingest directly as one series each.
inline bool isScalarArrowType(arrow::Type::type t) {
  return t == arrow::Type::BOOL || t == arrow::Type::INT8 || t == arrow::Type::INT16 ||
         t == arrow::Type::INT32 || t == arrow::Type::INT64 || t == arrow::Type::UINT8 ||
         t == arrow::Type::UINT16 || t == arrow::Type::UINT32 || t == arrow::Type::UINT64 ||
         t == arrow::Type::FLOAT || t == arrow::Type::DOUBLE || t == arrow::Type::TIMESTAMP ||
         t == arrow::Type::STRING || t == arrow::Type::LARGE_STRING;
}

/// True for `list<float|double>` / `fixed_size_list<float|double>` columns
/// (LeRobot observation.state / action are stored this way).
inline bool isFloatVectorColumn(const std::shared_ptr<arrow::DataType>& type) {
  std::shared_ptr<arrow::DataType> value_type;
  if (type->id() == arrow::Type::FIXED_SIZE_LIST) {
    value_type = std::static_pointer_cast<arrow::FixedSizeListType>(type)->value_type();
  } else if (type->id() == arrow::Type::LIST) {
    value_type = std::static_pointer_cast<arrow::ListType>(type)->value_type();
  } else if (type->id() == arrow::Type::LARGE_LIST) {
    value_type = std::static_pointer_cast<arrow::LargeListType>(type)->value_type();
  } else {
    return false;
  }
  return value_type->id() == arrow::Type::FLOAT || value_type->id() == arrow::Type::DOUBLE;
}

/// Map a scalar Arrow type to the PJ primitive used for field pre-registration.
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
    case arrow::Type::TIMESTAMP: return PJ::PrimitiveType::kInt64;
    case arrow::Type::STRING: return PJ::PrimitiveType::kString;
    case arrow::Type::LARGE_STRING: return PJ::PrimitiveType::kString;
    default: return PJ::PrimitiveType::kFloat64;
  }
}

/// Extract a native-typed ValueRef from a scalar Arrow array cell.
/// Returns NullValue for nulls / unsupported types. (Timestamps → ns int64;
/// LeRobot timestamps are synthesized separately, so no timezone handling.)
inline PJ::sdk::ValueRef getArrowValueRef(
    const std::shared_ptr<arrow::Array>& array, int64_t index, arrow::Type::type arrow_type) {
  if (array->IsNull(index)) {
    return PJ::NullValue{};
  }
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
      switch (ts_type->unit()) {
        case arrow::TimeUnit::SECOND: value *= 1'000'000'000LL; break;
        case arrow::TimeUnit::MILLI: value *= 1'000'000LL; break;
        case arrow::TimeUnit::MICRO: value *= 1'000LL; break;
        case arrow::TimeUnit::NANO: break;
      }
      return value;
    }
    case arrow::Type::STRING: {
      auto sv = std::static_pointer_cast<arrow::StringArray>(array)->GetView(index);
      return std::string_view(sv.data(), sv.size());
    }
    case arrow::Type::LARGE_STRING: {
      auto sv = std::static_pointer_cast<arrow::LargeStringArray>(array)->GetView(index);
      return std::string_view(sv.data(), sv.size());
    }
    default:
      return PJ::NullValue{};
  }
}

/// A vector-column cell viewed as its flat child float values for one row.
struct FloatVectorCell {
  std::shared_ptr<arrow::DoubleArray> d_values;  // set if child is double
  std::shared_ptr<arrow::FloatArray> f_values;   // set if child is float
  int64_t offset = 0;  // start index into the child array for this row
  int64_t width = 0;   // number of elements for this row

  [[nodiscard]] bool isNull(int64_t k) const {
    if (d_values) return d_values->IsNull(offset + k);
    if (f_values) return f_values->IsNull(offset + k);
    return true;
  }
  [[nodiscard]] double value(int64_t k) const {
    if (d_values) return d_values->Value(offset + k);
    if (f_values) return static_cast<double>(f_values->Value(offset + k));
    return 0.0;
  }
};

/// Resolve the child float values + [offset,width) for `row` of a
/// list/fixed_size_list float column. width==0 if the cell is null/empty.
inline FloatVectorCell floatVectorCell(const std::shared_ptr<arrow::Array>& array, int64_t row) {
  FloatVectorCell cell;
  std::shared_ptr<arrow::Array> child;
  if (array->type_id() == arrow::Type::FIXED_SIZE_LIST) {
    auto fsl = std::static_pointer_cast<arrow::FixedSizeListArray>(array);
    if (fsl->IsNull(row)) return cell;
    const int64_t w = fsl->value_length();
    cell.offset = (fsl->offset() + row) * w;
    cell.width = w;
    child = fsl->values();
  } else if (array->type_id() == arrow::Type::LIST) {
    auto la = std::static_pointer_cast<arrow::ListArray>(array);
    if (la->IsNull(row)) return cell;
    cell.offset = la->value_offset(row);
    cell.width = la->value_length(row);
    child = la->values();
  } else if (array->type_id() == arrow::Type::LARGE_LIST) {
    auto la = std::static_pointer_cast<arrow::LargeListArray>(array);
    if (la->IsNull(row)) return cell;
    cell.offset = la->value_offset(row);
    cell.width = la->value_length(row);
    child = la->values();
  } else {
    return cell;
  }
  if (child->type_id() == arrow::Type::DOUBLE) {
    cell.d_values = std::static_pointer_cast<arrow::DoubleArray>(child);
  } else if (child->type_id() == arrow::Type::FLOAT) {
    cell.f_values = std::static_pointer_cast<arrow::FloatArray>(child);
  } else {
    cell.width = 0;
  }
  return cell;
}

}  // namespace lerobot::arrow_helpers
