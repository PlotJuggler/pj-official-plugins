#include "pj_arrow_helpers/arrow_helpers.hpp"

namespace pj::arrow_helpers {

bool isSupportedArrowType(arrow::Type::type t) {
  return t == arrow::Type::BOOL || t == arrow::Type::INT8 || t == arrow::Type::INT16 || t == arrow::Type::INT32 ||
         t == arrow::Type::INT64 || t == arrow::Type::UINT8 || t == arrow::Type::UINT16 || t == arrow::Type::UINT32 ||
         t == arrow::Type::UINT64 || t == arrow::Type::FLOAT || t == arrow::Type::DOUBLE ||
         t == arrow::Type::TIMESTAMP || t == arrow::Type::STRING || t == arrow::Type::LARGE_STRING;
}

PJ::PrimitiveType arrowTypeToPrimitive(arrow::Type::type t) {
  switch (t) {
    case arrow::Type::BOOL:
      return PJ::PrimitiveType::kBool;
    case arrow::Type::INT8:
      return PJ::PrimitiveType::kInt8;
    case arrow::Type::INT16:
      return PJ::PrimitiveType::kInt16;
    case arrow::Type::INT32:
      return PJ::PrimitiveType::kInt32;
    case arrow::Type::INT64:
      return PJ::PrimitiveType::kInt64;
    case arrow::Type::UINT8:
      return PJ::PrimitiveType::kUint8;
    case arrow::Type::UINT16:
      return PJ::PrimitiveType::kUint16;
    case arrow::Type::UINT32:
      return PJ::PrimitiveType::kUint32;
    case arrow::Type::UINT64:
      return PJ::PrimitiveType::kUint64;
    case arrow::Type::FLOAT:
      return PJ::PrimitiveType::kFloat32;
    case arrow::Type::DOUBLE:
      return PJ::PrimitiveType::kFloat64;
    case arrow::Type::TIMESTAMP:
      return PJ::PrimitiveType::kInt64;  // nanoseconds
    case arrow::Type::STRING:
      return PJ::PrimitiveType::kString;
    case arrow::Type::LARGE_STRING:
      return PJ::PrimitiveType::kString;
    default:
      return PJ::PrimitiveType::kFloat64;
  }
}

PJ::sdk::ValueRef getArrowValueRef(
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
        case arrow::TimeUnit::SECOND:
          value *= 1'000'000'000LL;
          break;
        case arrow::TimeUnit::MILLI:
          value *= 1'000'000LL;
          break;
        case arrow::TimeUnit::MICRO:
          value *= 1'000LL;
          break;
        case arrow::TimeUnit::NANO:
          break;
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

}  // namespace pj::arrow_helpers
