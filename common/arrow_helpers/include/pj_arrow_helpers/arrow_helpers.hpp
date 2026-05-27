// Shared Arrow → PJ SDK conversion helpers.
//
// Extracted from data_load_parquet and data_load_lerobot so both plugins (and
// any future Arrow-consuming DataSource) share a single implementation. The
// helpers cover only the scalar cell extraction surface that is fully generic:
// type predicates, Arrow→PJ primitive mapping and a single-cell ValueRef
// extractor. Plugin-specific helpers (path utilities, timestamp heuristics,
// float-vector flattening) stay in their respective plugins.
#pragma once

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <pj_base/sdk/data_source_patterns.hpp>

namespace pj::arrow_helpers {

/// Scalar Arrow types this helper layer ingests directly as one series each.
bool isSupportedArrowType(arrow::Type::type t);

/// Map a scalar Arrow type to the PJ primitive used for field pre-registration.
PJ::PrimitiveType arrowTypeToPrimitive(arrow::Type::type t);

/// Extract a native-typed ValueRef from a scalar Arrow array cell.
/// Returns NullValue for nulls and unsupported types. Timestamps are returned
/// as int64 nanoseconds regardless of the Arrow time unit.
PJ::sdk::ValueRef getArrowValueRef(
    const std::shared_ptr<arrow::Array>& array, int64_t index, arrow::Type::type arrow_type);

}  // namespace pj::arrow_helpers
