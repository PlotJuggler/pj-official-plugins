// LeRobot-specific Arrow helpers: float `list<>` / `fixed_size_list<>`
// flattening. The generic scalar conversion helpers (isSupportedArrowType,
// arrowTypeToPrimitive, getArrowValueRef) live in the shared pj_arrow_helpers
// library — include that header directly from callsites that need them.
#pragma once

#include <arrow/api.h>

#include <cstdint>
#include <memory>

#include "pj_arrow_helpers/arrow_helpers.hpp"

namespace lerobot::arrow_helpers {

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

/// A vector-column cell viewed as its flat child float values for one row.
struct FloatVectorCell {
  std::shared_ptr<arrow::DoubleArray> d_values;  // set if child is double
  std::shared_ptr<arrow::FloatArray> f_values;   // set if child is float
  int64_t offset = 0;                            // start index into the child array for this row
  int64_t width = 0;                             // number of elements for this row

  [[nodiscard]] bool isNull(int64_t k) const {
    if (d_values) {
      return d_values->IsNull(offset + k);
    }
    if (f_values) {
      return f_values->IsNull(offset + k);
    }
    return true;
  }
  [[nodiscard]] double value(int64_t k) const {
    if (d_values) {
      return d_values->Value(offset + k);
    }
    if (f_values) {
      return static_cast<double>(f_values->Value(offset + k));
    }
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
    if (fsl->IsNull(row)) {
      return cell;
    }
    const int64_t w = fsl->value_length();
    cell.offset = (fsl->offset() + row) * w;
    cell.width = w;
    child = fsl->values();
  } else if (array->type_id() == arrow::Type::LIST) {
    auto la = std::static_pointer_cast<arrow::ListArray>(array);
    if (la->IsNull(row)) {
      return cell;
    }
    cell.offset = la->value_offset(row);
    cell.width = la->value_length(row);
    child = la->values();
  } else if (array->type_id() == arrow::Type::LARGE_LIST) {
    auto la = std::static_pointer_cast<arrow::LargeListArray>(array);
    if (la->IsNull(row)) {
      return cell;
    }
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
