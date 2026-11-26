// Arrow → PJ SDK conversion helpers shared by data_load_parquet and data_load_lerobot: scalar
// type predicates, Arrow→PJ primitive mapping and a single-cell ValueRef extractor.
#pragma once

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <pj_base/sdk/data_source_patterns.hpp>

namespace pj::arrow_helpers {

/// Scalar Arrow types this helper layer ingests directly as one series each.
bool isSupportedArrowType(arrow::Type::type t);

/// Maps a scalar Arrow type to the PJ primitive used for field pre-registration.
PJ::PrimitiveType arrowTypeToPrimitive(arrow::Type::type t);

/// Returns NullValue for nulls and unsupported types. Timestamps are returned as int64
/// nanoseconds regardless of the Arrow time unit.
PJ::sdk::ValueRef getArrowValueRef(
    const std::shared_ptr<arrow::Array>& array, int64_t index, arrow::Type::type arrow_type);

}  // namespace pj::arrow_helpers
