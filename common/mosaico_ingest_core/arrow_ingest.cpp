// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "arrow_ingest.hpp"

#include <arrow/api.h>
#include <arrow/array.h>
#include <arrow/array/array_binary.h>
#include <arrow/array/array_nested.h>
#include <arrow/array/array_primitive.h>
#include <arrow/compute/api.h>
#include <arrow/table.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "image_metadata.hpp"
#include "object_metadata.hpp"
#include "pj_base/builtin/frame_transforms.hpp"  // Vector3 / Quaternion / Pose
#include "pj_base/builtin/frame_transforms_codec.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_base/builtin/occupancy_grid_codec.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_base/builtin/poses_in_frame.hpp"
#include "pj_base/builtin/poses_in_frame_codec.hpp"
#include "pj_base/builtin/scene_entities.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"

namespace mosaico {

namespace {

// --------------------------------------------------------------------------
// Per-row Arrow column readers. Each dispatches on the chunk's runtime type so
// the same helper handles the int32/int64, BINARY/STRING and — critically for
// the Mosaico server — the BINARY_VIEW / STRING_VIEW variants Arrow >=15 emits
// for variable-length columns. Reading those view types with a plain
// BinaryArray/StringArray cast returns nothing, which is the root cause of
// image topics "barely downloading" (empty encoding/data -> the topic errored).
// --------------------------------------------------------------------------

[[nodiscard]] std::int32_t arrowI32At(const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  if (!col || row < 0 || row >= col->length()) {
    return 0;
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return 0;
      }
      switch (chunk->type_id()) {
        case arrow::Type::INT32:
          return std::static_pointer_cast<arrow::Int32Array>(chunk)->Value(chunk_row);
        case arrow::Type::UINT32:
          return static_cast<std::int32_t>(std::static_pointer_cast<arrow::UInt32Array>(chunk)->Value(chunk_row));
        case arrow::Type::INT64:
          return static_cast<std::int32_t>(std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(chunk_row));
        case arrow::Type::UINT64:
          return static_cast<std::int32_t>(std::static_pointer_cast<arrow::UInt64Array>(chunk)->Value(chunk_row));
        case arrow::Type::INT16:
          return std::static_pointer_cast<arrow::Int16Array>(chunk)->Value(chunk_row);
        case arrow::Type::UINT16:
          return std::static_pointer_cast<arrow::UInt16Array>(chunk)->Value(chunk_row);
        default:
          return 0;
      }
    }
    chunk_row -= chunk->length();
  }
  return 0;
}

// Read an int64-compatible scalar, returning std::nullopt on
// null/missing/out-of-range/unexpected-type so a caller can distinguish a real
// 0 from "no value" (e.g. a present-but-null timestamp cell, which must fall
// back to the synthetic timestamp rather than land the sample at epoch 0).
[[nodiscard]] std::optional<std::int64_t> arrowI64Opt(
    const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  if (!col || row < 0 || row >= col->length()) {
    return std::nullopt;
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return std::nullopt;
      }
      switch (chunk->type_id()) {
        case arrow::Type::INT64:
          return std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(chunk_row);
        case arrow::Type::UINT64:
          return static_cast<std::int64_t>(std::static_pointer_cast<arrow::UInt64Array>(chunk)->Value(chunk_row));
        case arrow::Type::INT32:
          return std::static_pointer_cast<arrow::Int32Array>(chunk)->Value(chunk_row);
        case arrow::Type::UINT32:
          return static_cast<std::int64_t>(std::static_pointer_cast<arrow::UInt32Array>(chunk)->Value(chunk_row));
        case arrow::Type::TIMESTAMP:
          return std::static_pointer_cast<arrow::TimestampArray>(chunk)->Value(chunk_row);
        default:
          return std::nullopt;
      }
    }
    chunk_row -= chunk->length();
  }
  return std::nullopt;
}

// Convenience wrapper: 0 on null/missing/unexpected-type. Use only where 0 is a
// safe default (geometry); for timestamps prefer arrowI64Opt + a synth fallback.
[[nodiscard]] std::int64_t arrowI64At(const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  return arrowI64Opt(col, row).value_or(0);
}

// Read a UTF-8 string handling STRING / LARGE_STRING / STRING_VIEW.
[[nodiscard]] std::string arrowStringAt(const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  if (!col || row < 0 || row >= col->length()) {
    return {};
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return {};
      }
      switch (chunk->type_id()) {
        case arrow::Type::STRING:
          return std::static_pointer_cast<arrow::StringArray>(chunk)->GetString(chunk_row);
        case arrow::Type::LARGE_STRING:
          return std::static_pointer_cast<arrow::LargeStringArray>(chunk)->GetString(chunk_row);
        case arrow::Type::STRING_VIEW:
          // Arrow >=15 emits Utf8View for variable-length string columns; the
          // Mosaico server uses it for `encoding`/`format`/`frame_id`.
          return std::static_pointer_cast<arrow::StringViewArray>(chunk)->GetString(chunk_row);
        default:
          return {};
      }
    }
    chunk_row -= chunk->length();
  }
  return {};
}

// Read a bool handling BOOL; returns @p fallback on null/missing/other type.
[[nodiscard]] bool arrowBoolAt(const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row, bool fallback) {
  if (!col || row < 0 || row >= col->length()) {
    return fallback;
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return fallback;
      }
      if (chunk->type_id() == arrow::Type::BOOL) {
        return std::static_pointer_cast<arrow::BooleanArray>(chunk)->Value(chunk_row);
      }
      return fallback;
    }
    chunk_row -= chunk->length();
  }
  return fallback;
}

// View a row's bytes as a span, handling BINARY / LARGE_BINARY /
// FIXED_SIZE_BINARY / BINARY_VIEW. The span borrows the Arrow buffer; the
// caller must not retain it past the lifetime of @p col.
[[nodiscard]] PJ::Span<const std::uint8_t> arrowBinaryAt(
    const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  if (!col || row < 0 || row >= col->length()) {
    return {};
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return {};
      }
      switch (chunk->type_id()) {
        case arrow::Type::BINARY: {
          auto bin = std::static_pointer_cast<arrow::BinaryArray>(chunk);
          int32_t length = 0;
          const std::uint8_t* ptr = bin->GetValue(chunk_row, &length);
          return PJ::Span<const std::uint8_t>(ptr, static_cast<std::size_t>(length));
        }
        case arrow::Type::LARGE_BINARY: {
          auto bin = std::static_pointer_cast<arrow::LargeBinaryArray>(chunk);
          int64_t length = 0;
          const std::uint8_t* ptr = bin->GetValue(chunk_row, &length);
          return PJ::Span<const std::uint8_t>(ptr, static_cast<std::size_t>(length));
        }
        case arrow::Type::FIXED_SIZE_BINARY: {
          auto bin = std::static_pointer_cast<arrow::FixedSizeBinaryArray>(chunk);
          return PJ::Span<const std::uint8_t>(bin->GetValue(chunk_row), static_cast<std::size_t>(bin->byte_width()));
        }
        case arrow::Type::BINARY_VIEW: {
          // Arrow >=15 emits BinaryView for variable-length binary columns;
          // the Mosaico server uses it for the image `data` column.
          auto bin = std::static_pointer_cast<arrow::BinaryViewArray>(chunk);
          const std::string_view view = bin->GetView(chunk_row);
          return PJ::Span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(view.data()), view.size());
        }
        default:
          return {};
      }
    }
    chunk_row -= chunk->length();
  }
  return {};
}

// Return the first column present under any of @p names, else nullptr.
[[nodiscard]] std::shared_ptr<arrow::ChunkedArray> firstPresentColumn(
    const std::shared_ptr<arrow::Table>& table, std::initializer_list<const char*> names) {
  for (const char* name : names) {
    if (auto col = table->GetColumnByName(name)) {
      return col;
    }
  }
  return nullptr;
}

// Read a scalar as double, handling DOUBLE / FLOAT / INT64 / INT32. Returns 0.0
// on null/missing/other type. Used for the per-row pose/transform struct-child
// columns (translation/x, rotation/w, …) after flattenStructColumns.
[[nodiscard]] double arrowDoubleAt(const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  if (!col || row < 0 || row >= col->length()) {
    return 0.0;
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return 0.0;
      }
      switch (chunk->type_id()) {
        case arrow::Type::DOUBLE:
          return std::static_pointer_cast<arrow::DoubleArray>(chunk)->Value(chunk_row);
        case arrow::Type::FLOAT:
          return static_cast<double>(std::static_pointer_cast<arrow::FloatArray>(chunk)->Value(chunk_row));
        case arrow::Type::INT64:
          return static_cast<double>(std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(chunk_row));
        case arrow::Type::INT32:
          return static_cast<double>(std::static_pointer_cast<arrow::Int32Array>(chunk)->Value(chunk_row));
        default:
          return 0.0;
      }
    }
    chunk_row -= chunk->length();
  }
  return 0.0;
}

// Forward declarations: readPointFields (next) reads the `fields` struct
// children through these type-flexible absolute-index helpers, which are defined
// further down with the other list/struct readers.
[[nodiscard]] std::string arrayStringAt(const std::shared_ptr<arrow::Array>& arr, std::int64_t idx);
[[nodiscard]] std::optional<std::int64_t> arrayIntAt(const std::shared_ptr<arrow::Array>& arr, std::int64_t idx);

// Read one row of the ROS-PointCloud2 `fields` column —
// list<struct<name:string, offset:uint32, datatype:int64, count:uint32>> — into
// canonical sdk::PointField records. Mosaico's `datatype` shares ROS PointField's
// numbering, which equals sdk::PointField::Datatype (kInt8=1 … kFloat64=8), so the
// value maps verbatim (out-of-range -> kUnknown). Returns empty on
// null/missing/wrong-shape.
[[nodiscard]] std::vector<PJ::sdk::PointField> readPointFields(
    const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  std::vector<PJ::sdk::PointField> out;
  if (!col || row < 0 || row >= col->length()) {
    return out;
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return out;
      }
      const auto list = std::dynamic_pointer_cast<arrow::ListArray>(chunk);
      if (!list) {
        return out;
      }
      const auto items = std::dynamic_pointer_cast<arrow::StructArray>(list->values());
      if (!items) {
        return out;
      }
      // Read each child through a type-flexible helper rather than a single
      // fixed cast: the `fields` column is a list<struct> that bypasses both
      // flattenStructColumns and normalizeViewColumns, so the server is free to
      // emit `name` as Utf8/LargeUtf8/Utf8View and offset/datatype/count at any
      // integer width. A fixed cast would null out and silently yield empty
      // names / offset 0 (every channel aliasing byte 0) — the exact view-type
      // failure this plugin already guards against on the scalar columns.
      const auto names = items->GetFieldByName("name");
      const auto offsets = items->GetFieldByName("offset");
      const auto datatypes = items->GetFieldByName("datatype");
      const auto counts = items->GetFieldByName("count");
      const std::int64_t begin = list->value_offset(chunk_row);
      const std::int64_t len = list->value_length(chunk_row);
      out.reserve(static_cast<std::size_t>(len));
      for (std::int64_t k = begin; k < begin + len; ++k) {
        PJ::sdk::PointField field;
        field.name = arrayStringAt(names, k);
        field.offset = static_cast<std::uint32_t>(arrayIntAt(offsets, k).value_or(0));
        const std::int64_t dt = arrayIntAt(datatypes, k).value_or(0);
        field.datatype = (dt >= 1 && dt <= 8) ? static_cast<PJ::sdk::PointField::Datatype>(dt)
                                              : PJ::sdk::PointField::Datatype::kUnknown;
        const std::int64_t count = arrayIntAt(counts, k).value_or(1);
        field.count = count > 0 ? static_cast<std::uint32_t>(count) : 1U;
        out.push_back(std::move(field));
      }
      return out;
    }
    chunk_row -= chunk->length();
  }
  return out;
}

// --------------------------------------------------------------------------
// List / struct readers for the non-packed object ontologies (transform,
// occupancy_grid, laser_scan, grid_cells, and the columnar "futures" clouds).
// These mirror the per-row chunk-walk of the scalar readers above but descend
// into Arrow list<…> / struct<…> values.
// --------------------------------------------------------------------------

// Read a UTF-8 string at an ABSOLUTE index of an (already-resolved) Array,
// handling STRING / LARGE_STRING / STRING_VIEW. For list/struct children whose
// parent chunk has already been located.
[[nodiscard]] std::string arrayStringAt(const std::shared_ptr<arrow::Array>& arr, std::int64_t idx) {
  if (!arr || idx < 0 || idx >= arr->length() || arr->IsNull(idx)) {
    return {};
  }
  switch (arr->type_id()) {
    case arrow::Type::STRING:
      return std::static_pointer_cast<arrow::StringArray>(arr)->GetString(idx);
    case arrow::Type::LARGE_STRING:
      return std::static_pointer_cast<arrow::LargeStringArray>(arr)->GetString(idx);
    case arrow::Type::STRING_VIEW:
      return std::static_pointer_cast<arrow::StringViewArray>(arr)->GetString(idx);
    default:
      return {};
  }
}

// Read an integer Array child at an ABSOLUTE index, handling every signed /
// unsigned width (8..64). Returns std::nullopt on null/missing/non-integer so
// the caller can tell "absent" from a real 0. Used for the PointCloud2 `fields`
// descriptor children (offset / datatype / count), whose Arrow width the server
// is free to choose — a single fixed cast would silently yield 0 for any other
// width.
[[nodiscard]] std::optional<std::int64_t> arrayIntAt(const std::shared_ptr<arrow::Array>& arr, std::int64_t idx) {
  if (!arr || idx < 0 || idx >= arr->length() || arr->IsNull(idx)) {
    return std::nullopt;
  }
  switch (arr->type_id()) {
    case arrow::Type::INT8:
      return std::static_pointer_cast<arrow::Int8Array>(arr)->Value(idx);
    case arrow::Type::UINT8:
      return std::static_pointer_cast<arrow::UInt8Array>(arr)->Value(idx);
    case arrow::Type::INT16:
      return std::static_pointer_cast<arrow::Int16Array>(arr)->Value(idx);
    case arrow::Type::UINT16:
      return std::static_pointer_cast<arrow::UInt16Array>(arr)->Value(idx);
    case arrow::Type::INT32:
      return std::static_pointer_cast<arrow::Int32Array>(arr)->Value(idx);
    case arrow::Type::UINT32:
      return std::static_pointer_cast<arrow::UInt32Array>(arr)->Value(idx);
    case arrow::Type::INT64:
      return std::static_pointer_cast<arrow::Int64Array>(arr)->Value(idx);
    case arrow::Type::UINT64:
      return static_cast<std::int64_t>(std::static_pointer_cast<arrow::UInt64Array>(arr)->Value(idx));
    default:
      return std::nullopt;
  }
}

// A rotation quaternion whose components all defaulted to 0 (missing/null/typed
// unexpectedly so every read fell back to 0.0) is not a valid rotation — its
// norm is 0 and downstream TF/pose math would divide by it. Substitute identity
// (w=1) so a server schema surprise degrades to "no rotation" rather than NaNs.
[[nodiscard]] PJ::sdk::Quaternion sanitizeQuaternion(PJ::sdk::Quaternion q) {
  const double norm2 = (q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w);
  if (!std::isfinite(norm2) || norm2 < 1e-12) {
    return PJ::sdk::Quaternion{.x = 0.0, .y = 0.0, .z = 0.0, .w = 1.0};
  }
  return q;
}

// Read a struct child as double at an ABSOLUTE index, handling DOUBLE / FLOAT /
// INT*. Returns 0.0 when the child is absent/null/non-numeric.
[[nodiscard]] double structChildDoubleAt(const std::shared_ptr<arrow::Array>& s, const char* name, std::int64_t idx) {
  const auto st = std::dynamic_pointer_cast<arrow::StructArray>(s);
  if (!st) {
    return 0.0;
  }
  const auto child = st->GetFieldByName(name);
  if (!child || idx < 0 || idx >= child->length() || child->IsNull(idx)) {
    return 0.0;
  }
  switch (child->type_id()) {
    case arrow::Type::DOUBLE:
      return std::static_pointer_cast<arrow::DoubleArray>(child)->Value(idx);
    case arrow::Type::FLOAT:
      return static_cast<double>(std::static_pointer_cast<arrow::FloatArray>(child)->Value(idx));
    case arrow::Type::INT64:
      return static_cast<double>(std::static_pointer_cast<arrow::Int64Array>(child)->Value(idx));
    case arrow::Type::INT32:
      return static_cast<double>(std::static_pointer_cast<arrow::Int32Array>(child)->Value(idx));
    default:
      return 0.0;
  }
}

// Read a struct child string ("frame_id" inside a header struct) at an ABSOLUTE index.
[[nodiscard]] std::string structChildStringAt(
    const std::shared_ptr<arrow::Array>& s, const char* name, std::int64_t idx) {
  const auto st = std::dynamic_pointer_cast<arrow::StructArray>(s);
  if (!st) {
    return {};
  }
  return arrayStringAt(st->GetFieldByName(name), idx);
}

// One per-point attribute list read out of a single row, kept as contiguous
// native bytes (count * bytesPerElement) tagged with its canonical PointField
// datatype. Backs the columnar→packed PointCloud build.
struct NumericList {
  PJ::sdk::PointField::Datatype datatype = PJ::sdk::PointField::Datatype::kUnknown;
  std::int64_t count = 0;
  std::vector<std::uint8_t> bytes;
  [[nodiscard]] bool valid() const {
    return datatype != PJ::sdk::PointField::Datatype::kUnknown && count > 0;
  }
};

template <typename ArrayT, typename ElemT>
void copyListElems(
    const std::shared_ptr<arrow::Array>& values, std::int64_t begin, std::int64_t len, std::vector<std::uint8_t>& out) {
  const auto a = std::static_pointer_cast<ArrayT>(values);
  out.resize(static_cast<std::size_t>(len) * sizeof(ElemT));
  for (std::int64_t k = 0; k < len; ++k) {
    const ElemT v = a->Value(begin + k);
    std::memcpy(out.data() + static_cast<std::size_t>(k) * sizeof(ElemT), &v, sizeof(ElemT));
  }
}

// Locate the list values + [begin, len) slice for @p row of a LIST / LARGE_LIST
// chunked column. Returns false (and leaves outputs untouched) on
// null/missing/non-list/empty.
[[nodiscard]] bool resolveListSlice(
    const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row, std::shared_ptr<arrow::Array>& values,
    std::int64_t& begin, std::int64_t& len) {
  if (!col || row < 0 || row >= col->length()) {
    return false;
  }
  std::int64_t chunk_row = row;
  for (int i = 0; i < col->num_chunks(); ++i) {
    const auto& chunk = col->chunk(i);
    if (chunk_row < chunk->length()) {
      if (chunk->IsNull(chunk_row)) {
        return false;
      }
      if (const auto l = std::dynamic_pointer_cast<arrow::ListArray>(chunk)) {
        values = l->values();
        begin = l->value_offset(chunk_row);
        len = l->value_length(chunk_row);
      } else if (const auto ll = std::dynamic_pointer_cast<arrow::LargeListArray>(chunk)) {
        values = ll->values();
        begin = ll->value_offset(chunk_row);
        len = ll->value_length(chunk_row);
      } else {
        return false;
      }
      return values != nullptr && len > 0;
    }
    chunk_row -= chunk->length();
  }
  return false;
}

// Read one row of a list<primitive> column into a NumericList. Handles
// FLOAT/DOUBLE/INT8/UINT8/INT16/UINT16/INT32/UINT32 elements (the canonical
// PointField datatypes); other element types yield an invalid (empty) result.
[[nodiscard]] NumericList readNumericList(const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  NumericList out;
  std::shared_ptr<arrow::Array> values;
  std::int64_t begin = 0;
  std::int64_t len = 0;
  if (!resolveListSlice(col, row, values, begin, len)) {
    return out;
  }
  using DT = PJ::sdk::PointField::Datatype;
  switch (values->type_id()) {
    case arrow::Type::FLOAT:
      out.datatype = DT::kFloat32;
      copyListElems<arrow::FloatArray, float>(values, begin, len, out.bytes);
      break;
    case arrow::Type::DOUBLE:
      out.datatype = DT::kFloat64;
      copyListElems<arrow::DoubleArray, double>(values, begin, len, out.bytes);
      break;
    case arrow::Type::INT8:
      out.datatype = DT::kInt8;
      copyListElems<arrow::Int8Array, std::int8_t>(values, begin, len, out.bytes);
      break;
    case arrow::Type::UINT8:
      out.datatype = DT::kUint8;
      copyListElems<arrow::UInt8Array, std::uint8_t>(values, begin, len, out.bytes);
      break;
    case arrow::Type::INT16:
      out.datatype = DT::kInt16;
      copyListElems<arrow::Int16Array, std::int16_t>(values, begin, len, out.bytes);
      break;
    case arrow::Type::UINT16:
      out.datatype = DT::kUint16;
      copyListElems<arrow::UInt16Array, std::uint16_t>(values, begin, len, out.bytes);
      break;
    case arrow::Type::INT32:
      out.datatype = DT::kInt32;
      copyListElems<arrow::Int32Array, std::int32_t>(values, begin, len, out.bytes);
      break;
    case arrow::Type::UINT32:
      out.datatype = DT::kUint32;
      copyListElems<arrow::UInt32Array, std::uint32_t>(values, begin, len, out.bytes);
      break;
    default:
      return out;
  }
  out.count = len;
  return out;
}

// Decode element @p i of a NumericList to double (used where a uniform numeric
// view is enough, e.g. laser ranges).
[[nodiscard]] double numericElemAsDouble(const NumericList& nl, std::int64_t i) {
  if (i < 0 || i >= nl.count) {
    return 0.0;
  }
  const std::uint8_t* p = nl.bytes.data() + static_cast<std::size_t>(i) * PJ::sdk::bytesPerElement(nl.datatype);
  using DT = PJ::sdk::PointField::Datatype;
  switch (nl.datatype) {
    case DT::kFloat32: {
      float v = 0.0F;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case DT::kFloat64: {
      double v = 0.0;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case DT::kInt8:
      return static_cast<double>(static_cast<std::int8_t>(*p));
    case DT::kUint8:
      return static_cast<double>(*p);
    case DT::kInt16: {
      std::int16_t v = 0;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case DT::kUint16: {
      std::uint16_t v = 0;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case DT::kInt32: {
      std::int32_t v = 0;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case DT::kUint32: {
      std::uint32_t v = 0;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<double>(v);
    }
    case DT::kUnknown:
    default:
      return 0.0;
  }
}

// Read one row of a list<struct{…,x,y,z,…}> column into XYZ triples (e.g. the
// grid_cells `cells` column; the structs carry mixin baggage we ignore).
[[nodiscard]] std::vector<std::array<double, 3>> readXyzStructList(
    const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  std::vector<std::array<double, 3>> out;
  std::shared_ptr<arrow::Array> values;
  std::int64_t begin = 0;
  std::int64_t len = 0;
  if (!resolveListSlice(col, row, values, begin, len)) {
    return out;
  }
  out.reserve(static_cast<std::size_t>(len));
  for (std::int64_t k = begin; k < begin + len; ++k) {
    out.push_back(
        std::array<double, 3>{
            structChildDoubleAt(values, "x", k), structChildDoubleAt(values, "y", k),
            structChildDoubleAt(values, "z", k)});
  }
  return out;
}

// Read one row of a `frame_transform` `transforms` list<struct{…}> column into
// FrameTransform records (parent_frame_id = header.frame_id, child_frame_id =
// target_frame_id). The caller fills each record's timestamp.
[[nodiscard]] std::vector<PJ::sdk::FrameTransform> readTransformList(
    const std::shared_ptr<arrow::ChunkedArray>& col, std::int64_t row) {
  std::vector<PJ::sdk::FrameTransform> out;
  std::shared_ptr<arrow::Array> values;
  std::int64_t begin = 0;
  std::int64_t len = 0;
  if (!resolveListSlice(col, row, values, begin, len)) {
    return out;
  }
  const auto items = std::dynamic_pointer_cast<arrow::StructArray>(values);
  if (!items) {
    return out;
  }
  const auto translation = items->GetFieldByName("translation");
  const auto rotation = items->GetFieldByName("rotation");
  const auto target = items->GetFieldByName("target_frame_id");
  const auto header = items->GetFieldByName("header");
  out.reserve(static_cast<std::size_t>(len));
  for (std::int64_t k = begin; k < begin + len; ++k) {
    PJ::sdk::FrameTransform ft;
    ft.translation = PJ::sdk::Vector3{
        .x = structChildDoubleAt(translation, "x", k),
        .y = structChildDoubleAt(translation, "y", k),
        .z = structChildDoubleAt(translation, "z", k)};
    ft.rotation = sanitizeQuaternion(
        PJ::sdk::Quaternion{
            .x = structChildDoubleAt(rotation, "x", k),
            .y = structChildDoubleAt(rotation, "y", k),
            .z = structChildDoubleAt(rotation, "z", k),
            .w = structChildDoubleAt(rotation, "w", k)});
    ft.child_frame_id = arrayStringAt(target, k);
    ft.parent_frame_id = structChildStringAt(header, "frame_id", k);
    out.push_back(std::move(ft));
  }
  return out;
}

// One attribute feeding the packed-cloud builder.
struct PackAttr {
  std::string name;
  const NumericList* src = nullptr;
};

// Pack parallel per-point attributes into a canonical PointCloud `data` buffer.
// Fills @p cloud.fields/width/height/point_step/row_step and returns the packed
// bytes (the caller points cloud.data at them and keeps them alive until
// serialize copies them). Each attribute contributes one PointField of its
// native datatype; tail elements beyond an attribute's count are left zero.
[[nodiscard]] std::vector<std::uint8_t> packPointCloud(
    PJ::sdk::PointCloud& cloud, const std::vector<PackAttr>& attrs, std::int64_t npoints) {
  std::uint32_t point_step = 0;
  cloud.fields.clear();
  cloud.fields.reserve(attrs.size());
  for (const auto& a : attrs) {
    PJ::sdk::PointField f;
    f.name = a.name;
    f.offset = point_step;
    f.datatype = a.src->datatype;
    f.count = 1;
    cloud.fields.push_back(std::move(f));
    point_step += PJ::sdk::bytesPerElement(a.src->datatype);
  }
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(npoints) * point_step);
  for (std::int64_t i = 0; i < npoints; ++i) {
    std::uint32_t off = 0;
    for (const auto& a : attrs) {
      const std::uint32_t esz = PJ::sdk::bytesPerElement(a.src->datatype);
      if (i < a.src->count) {
        std::memcpy(
            buf.data() + static_cast<std::size_t>(i) * point_step + off,
            a.src->bytes.data() + static_cast<std::size_t>(i) * esz, esz);
      }
      off += esz;
    }
  }
  cloud.width = static_cast<std::uint32_t>(npoints);
  cloud.height = 1;
  cloud.point_step = point_step;
  cloud.row_step = point_step * cloud.width;
  return buf;
}

}  // namespace

std::string detectTimestampColumn(const ArrowSchema* schema) {
  if (schema == nullptr || schema->children == nullptr) {
    return {};
  }
  // First pass: Arrow TIMESTAMP type. The format string for timestamps
  // starts with "ts" per Arrow C ABI spec (e.g. "tsn:UTC").
  for (int64_t i = 0; i < schema->n_children; ++i) {
    const auto* child = schema->children[i];
    if (child != nullptr && child->format != nullptr) {
      std::string_view fmt(child->format);
      if (fmt.size() >= 2 && fmt[0] == 't' && fmt[1] == 's') {
        return child->name != nullptr ? std::string(child->name) : std::string();
      }
    }
  }
  // Second pass: name heuristics. Order matters — most-specific first.
  static const std::array<std::string_view, 5> kNames = {
      "timestamp_ns", "recording_timestamp_ns", "timestamp", "time", "ts"};
  for (std::string_view preferred : kNames) {
    for (int64_t i = 0; i < schema->n_children; ++i) {
      const auto* child = schema->children[i];
      if (child != nullptr && child->name != nullptr && std::string_view(child->name) == preferred) {
        return std::string(child->name);
      }
    }
  }
  return {};
}

arrow::Result<std::shared_ptr<arrow::Table>> normalizeViewColumns(std::shared_ptr<arrow::Table> table) {
  if (!table) {
    return arrow::Status::Invalid("normalizeViewColumns: null table");
  }
  // Cast Arrow "view" string/binary columns (Utf8View / BinaryView, which the
  // Mosaico server / Arrow >= 15 emit for e.g. frame_id) to canonical Utf8 /
  // Binary. pj_datastore's nanoarrow import maps only STRING / LARGE_STRING; a
  // view-typed column anywhere in the record batch corrupts the import for the
  // WHOLE batch, so every column (even plain doubles) lands as null. Applied to
  // the scalar (appendArrowStream) pipeline only — the image path reads view
  // types directly.
  std::vector<std::shared_ptr<arrow::ChunkedArray>> cols = table->columns();
  std::vector<std::shared_ptr<arrow::Field>> fields;
  fields.reserve(static_cast<std::size_t>(table->num_columns()));
  bool changed = false;
  for (int i = 0; i < table->num_columns(); ++i) {
    const auto& field = table->schema()->field(i);
    std::shared_ptr<arrow::DataType> target;
    if (field->type()->id() == arrow::Type::STRING_VIEW) {
      target = arrow::utf8();
    } else if (field->type()->id() == arrow::Type::BINARY_VIEW) {
      target = arrow::binary();
    }
    if (target != nullptr) {
      ARROW_ASSIGN_OR_RAISE(auto casted, arrow::compute::Cast(arrow::Datum(cols[static_cast<std::size_t>(i)]), target));
      cols[static_cast<std::size_t>(i)] = casted.chunked_array();
      fields.push_back(field->WithType(target));
      changed = true;
    } else {
      fields.push_back(field);
    }
  }
  if (!changed) {
    return table;
  }
  return arrow::Table::Make(std::make_shared<arrow::Schema>(fields), cols, table->num_rows());
}

arrow::Result<std::shared_ptr<arrow::Table>> flattenStructColumns(std::shared_ptr<arrow::Table> table) {
  if (!table) {
    return arrow::Status::Invalid("flattenStructColumns: null table");
  }

  // Arrow's Table::Flatten only peels one struct layer at a time and uses
  // "." as the separator. Loop until there are no more struct columns, then
  // rewrite the dotted names with "/" so the resulting curve paths match
  // PJ3's flattenArray output (toolbox_mosaico.cpp:374).
  auto current = std::move(table);
  while (true) {
    bool has_struct = false;
    for (int i = 0; i < current->num_columns(); ++i) {
      if (current->schema()->field(i)->type()->id() == arrow::Type::STRUCT) {
        has_struct = true;
        break;
      }
    }
    if (!has_struct) {
      break;
    }
    ARROW_ASSIGN_OR_RAISE(current, current->Flatten());
  }

  // Rename "parent.child" → "parent/child" to keep curve keys consistent
  // with PJ3 (e.g. "<sequence>/<topic>/pose/position/x"). Only the fields
  // change — the data and chunk layout carry through untouched.
  std::vector<std::shared_ptr<arrow::Field>> renamed_fields;
  renamed_fields.reserve(static_cast<std::size_t>(current->num_columns()));
  bool needs_rename = false;
  for (int i = 0; i < current->num_columns(); ++i) {
    const auto& field = current->schema()->field(i);
    if (field->name().find('.') == std::string::npos) {
      renamed_fields.push_back(field);
      continue;
    }
    needs_rename = true;
    std::string new_name = field->name();
    std::replace(new_name.begin(), new_name.end(), '.', '/');
    renamed_fields.push_back(arrow::field(new_name, field->type(), field->nullable(), field->metadata()));
  }
  if (needs_rename) {
    // Carry the original table-level metadata onto the rebuilt schema. It holds
    // `mosaico:properties` (the ontology_tag) that resolveOntologyTag falls back
    // on; the default Schema ctor would drop it, silently routing struct-bearing
    // ontologies (pose/transform/occupancy_grid) to the scalar path whenever the
    // cached tag is unavailable.
    auto renamed_schema = std::make_shared<arrow::Schema>(renamed_fields, current->schema()->metadata());
    current = arrow::Table::Make(renamed_schema, current->columns(), current->num_rows());
  }

  // Coalesce per-column chunks into one. Table::Flatten extracts struct
  // children as Arrays that reference the parent struct's buffers — when
  // those reach ExportRecordBatchReader, the C ABI batch carries the data
  // pointer at offset 0 but the conceptual data lives at the parent's
  // slice offset. The result downstream: pj_datastore's importArrowStream
  // reads from the wrong slot and every numeric column lands as zeros.
  // CombineChunks normalizes each column into a single contiguous chunk
  // with offset = 0, which exports cleanly.
  return current->CombineChunks();
}

PJ::Status pumpStreamToHost(
    const PJ::sdk::ToolboxHostView& host, PJ::sdk::DataSourceHandle source, std::string_view topic_name,
    ArrowArrayStream* stream, std::string_view timestamp_col) {
  if (!host.valid()) {
    return PJ::unexpected("arrow_ingest: toolbox host not bound");
  }
  if (stream == nullptr) {
    return PJ::unexpected("arrow_ingest: null stream");
  }
  // The data source already carries the sequence name (it is created once per
  // Download in FetchWorker::datasetForFetch and shared by every topic), so the
  // topic is registered under its BARE name. The resulting catalog tree is
  // <sequence> ▸ <topic> ▸ fields.
  auto topic = host.ensureTopic(source, topic_name);
  if (!topic) {
    return PJ::unexpected(std::move(topic).error());
  }
  return host.appendArrowStream(*topic, stream, timestamp_col);
}

PJ::Expected<ImagePushOutcome> pushImageRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table) {
  const PJ::sdk::ToolboxHostView& host = ctx.host;
  const PJ::sdk::DataSourceHandle source = ctx.source;
  const std::string& topic_name = ctx.topic_name;
  const std::string& ts_field = ctx.ts_field;
  const std::int64_t synth_anchor_ns = ctx.synth_anchor_ns;
  const std::int64_t synth_interval_ns = ctx.synth_interval_ns;
  if (!host.valid()) {
    return PJ::unexpected(std::string("toolbox host not bound"));
  }
  if (!table) {
    return PJ::unexpected(std::string("image topic '") + topic_name + "': null table");
  }
  const auto data_col = table->GetColumnByName("data");
  if (!data_col) {
    return PJ::unexpected(std::string("image topic '") + topic_name + "' missing 'data' column");
  }
  // Resolve columns once; geometry/encoding are read per-row below.
  const auto width_col = table->GetColumnByName("width");
  const auto height_col = table->GetColumnByName("height");
  const auto stride_col = firstPresentColumn(table, {"stride", "step", "row_step"});
  const auto encoding_col = table->GetColumnByName("encoding");
  const auto format_col = table->GetColumnByName("format");
  const auto bigendian_col = table->GetColumnByName("is_bigendian");
  const auto ts_col = ts_field.empty() ? nullptr : table->GetColumnByName(ts_field);

  // Register the object topic ONCE under its BARE name on the shared data
  // source. The catalog tree is <sequence> ▸ <topic> ▸ image, grouped with the
  // scalar siblings.
  auto topic_handle = host.registerObjectTopic(source, topic_name, kCanonicalImageMetadata);
  if (!topic_handle) {
    return PJ::unexpected(std::move(topic_handle).error());
  }

  ImagePushOutcome outcome;
  const std::int64_t num_rows = table->num_rows();
  for (std::int64_t row = 0; row < num_rows; ++row) {
    const auto bytes_span = arrowBinaryAt(data_col, row);
    if (bytes_span.empty()) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error =
            std::string("image topic '") + topic_name + "' row " + std::to_string(row) + ": missing/empty 'data'";
      }
      continue;
    }

    const std::int32_t width = arrowI32At(width_col, row);
    const std::int32_t height = arrowI32At(height_col, row);
    const std::int32_t stride = arrowI32At(stride_col, row);
    std::string encoding = arrowStringAt(encoding_col, row);
    // Capture emptiness of the per-row `encoding` BEFORE the `format` fallback
    // overwrites it — a row that arrived with no pixel `encoding` is a
    // pure-compressed frame (geometry lives inside the blob). Reused below for
    // the is_compressed test so we don't re-materialize the string column.
    const bool encoding_was_empty = encoding.empty();
    const std::string format = arrowStringAt(format_col, row);
    // Pure-compressed topics ship only `format` (jpeg/png) with no pixel
    // `encoding`; fall back so the blob still carries a usable encoding.
    if (encoding.empty()) {
      encoding = format;
    }
    if (encoding.empty()) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error = std::string("image topic '") + topic_name + "' row " + std::to_string(row) +
                              ": missing both 'encoding' and 'format'";
      }
      continue;
    }
    // A raw (non-compressed) frame needs positive geometry. A compressed frame
    // (encoding came from `format`, geometry lives inside the blob) is allowed
    // to have width/height == 0.
    const bool is_compressed = encoding_col == nullptr || encoding_was_empty;
    if (!is_compressed && (width <= 0 || height <= 0)) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error = std::string("image topic '") + topic_name + "' row " + std::to_string(row) +
                              ": non-positive geometry (width=" + std::to_string(width) +
                              " height=" + std::to_string(height) + ")";
      }
      continue;
    }

    std::int64_t ts_ns = ts_col ? arrowI64Opt(ts_col, row).value_or(synth_anchor_ns + row * synth_interval_ns)
                                : (synth_anchor_ns + row * synth_interval_ns);

    PJ::sdk::Image img;
    img.width = width > 0 ? static_cast<std::uint32_t>(width) : 0U;
    img.height = height > 0 ? static_cast<std::uint32_t>(height) : 0U;
    img.row_step = stride > 0 ? static_cast<std::uint32_t>(stride) : 0U;
    img.encoding = std::move(encoding);
    // is_bigendian describes the PRODUCER's byte order and matters only for
    // multi-byte raw encodings (mono16). When the column is null/absent we
    // default to false (little-endian), matching the canonical sdk::Image
    // struct default (Image.hpp) and the ROS convention. An explicit column
    // value is always honored.
    img.is_bigendian = arrowBoolAt(bigendian_col, row, /*fallback=*/false);
    img.timestamp_ns = ts_ns;
    img.data = bytes_span;  // borrowed; serializeImage copies the bytes.

    const std::vector<std::uint8_t> blob = PJ::serializeImage(img);
    auto status = host.pushOwnedObject(*topic_handle, ts_ns, PJ::Span<const std::uint8_t>(blob.data(), blob.size()));
    if (!status) {
      return PJ::unexpected(std::move(status).error());
    }
    teeIfPresent(ctx, ts_ns, blob);
    ++outcome.pushed;
  }
  return outcome;
}

PJ::Expected<ObjectPushOutcome> pushPointCloudRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table) {
  const PJ::sdk::ToolboxHostView& host = ctx.host;
  const PJ::sdk::DataSourceHandle source = ctx.source;
  const std::string& topic_name = ctx.topic_name;
  const std::string& ts_field = ctx.ts_field;
  const std::int64_t synth_anchor_ns = ctx.synth_anchor_ns;
  const std::int64_t synth_interval_ns = ctx.synth_interval_ns;
  if (!host.valid()) {
    return PJ::unexpected(std::string("toolbox host not bound"));
  }
  if (!table) {
    return PJ::unexpected(std::string("point_cloud topic '") + topic_name + "': null table");
  }
  // The Mosaico `point_cloud2` ontology is ROS PointCloud2-shaped: a single
  // packed `data` blob plus a `fields` channel descriptor and
  // point_step/row_step/width/height — which IS the canonical sdk::PointCloud
  // layout. So this copies the fields through verbatim: no point packing and,
  // crucially, no decode/decompression (the bytes are already the canonical
  // point buffer; host-side coloring/conversion happens in pj_scene3D).
  const auto data_col = table->GetColumnByName("data");
  const auto fields_col = table->GetColumnByName("fields");
  if (!data_col || !fields_col) {
    return PJ::unexpected(std::string("point_cloud topic '") + topic_name + "' missing 'data' or 'fields' column");
  }
  const auto width_col = table->GetColumnByName("width");
  const auto height_col = table->GetColumnByName("height");
  const auto point_step_col = table->GetColumnByName("point_step");
  const auto row_step_col = table->GetColumnByName("row_step");
  const auto bigendian_col = table->GetColumnByName("is_bigendian");
  const auto dense_col = table->GetColumnByName("is_dense");
  const auto frame_col = table->GetColumnByName("frame_id");
  const auto ts_col = ts_field.empty() ? nullptr : table->GetColumnByName(ts_field);

  auto topic_handle = host.registerObjectTopic(source, topic_name, kCanonicalPointCloudMetadata);
  if (!topic_handle) {
    return PJ::unexpected(std::move(topic_handle).error());
  }

  ObjectPushOutcome outcome;
  const std::int64_t num_rows = table->num_rows();
  for (std::int64_t row = 0; row < num_rows; ++row) {
    const auto bytes_span = arrowBinaryAt(data_col, row);
    std::vector<PJ::sdk::PointField> fields = readPointFields(fields_col, row);
    if (bytes_span.empty() || fields.empty()) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error = std::string("point_cloud topic '") + topic_name + "' row " + std::to_string(row) +
                              ": empty 'data' or 'fields'";
      }
      continue;
    }
    const std::int64_t ts_ns = ts_col ? arrowI64Opt(ts_col, row).value_or(synth_anchor_ns + row * synth_interval_ns)
                                      : (synth_anchor_ns + row * synth_interval_ns);
    const std::int32_t width = arrowI32At(width_col, row);
    const std::int32_t point_step = arrowI32At(point_step_col, row);
    const std::int32_t row_step = arrowI32At(row_step_col, row);

    PJ::sdk::PointCloud cloud;
    cloud.width = width > 0 ? static_cast<std::uint32_t>(width) : 0U;
    cloud.height = static_cast<std::uint32_t>(std::max<std::int32_t>(arrowI32At(height_col, row), 1));
    cloud.point_step = point_step > 0 ? static_cast<std::uint32_t>(point_step) : 0U;
    cloud.row_step = row_step > 0 ? static_cast<std::uint32_t>(row_step) : cloud.point_step * cloud.width;
    cloud.is_bigendian = arrowBoolAt(bigendian_col, row, /*fallback=*/false);
    cloud.is_dense = arrowBoolAt(dense_col, row, /*fallback=*/true);
    cloud.frame_id = frame_col ? arrowStringAt(frame_col, row) : std::string{};
    cloud.fields = std::move(fields);
    cloud.data = bytes_span;  // borrowed; serializePointCloud copies the bytes.
    cloud.timestamp_ns = ts_ns;

    const std::vector<std::uint8_t> blob = PJ::serializePointCloud(cloud);
    auto status = host.pushOwnedObject(*topic_handle, ts_ns, PJ::Span<const std::uint8_t>(blob.data(), blob.size()));
    if (!status) {
      return PJ::unexpected(std::move(status).error());
    }
    teeIfPresent(ctx, ts_ns, blob);
    ++outcome.pushed;
  }
  return outcome;
}

PJ::Expected<ObjectPushOutcome> pushPoseRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table) {
  const PJ::sdk::ToolboxHostView& host = ctx.host;
  const PJ::sdk::DataSourceHandle source = ctx.source;
  const std::string& topic_name = ctx.topic_name;
  const std::string& ts_field = ctx.ts_field;
  const std::int64_t synth_anchor_ns = ctx.synth_anchor_ns;
  const std::int64_t synth_interval_ns = ctx.synth_interval_ns;
  if (!host.valid()) {
    return PJ::unexpected(std::string("toolbox host not bound"));
  }
  if (!table) {
    return PJ::unexpected(std::string("pose topic '") + topic_name + "': null table");
  }
  // position/orientation arrive as struct columns; flatten to position/x …. The
  // `pose` ontology nests them at the top level (position/*); `motion_state`
  // (odometry) nests them under pose/* (pose/position/*) — accept either prefix.
  auto flat_res = flattenStructColumns(table);
  if (!flat_res.ok()) {
    return PJ::unexpected(
        std::string("pose topic '") + topic_name + "': flatten failed: " + flat_res.status().ToString());
  }
  const std::shared_ptr<arrow::Table> flat = *flat_res;
  std::string prefix;
  if (flat->GetColumnByName("position/x")) {
    prefix = "";
  } else if (flat->GetColumnByName("pose/position/x")) {
    prefix = "pose/";  // motion_state (odometry) nests the pose under pose/*
  } else {
    return PJ::unexpected(std::string("pose topic '") + topic_name + "' missing position/* columns");
  }
  const auto px = flat->GetColumnByName(prefix + "position/x");
  const auto py = flat->GetColumnByName(prefix + "position/y");
  const auto pz = flat->GetColumnByName(prefix + "position/z");
  const auto ox = flat->GetColumnByName(prefix + "orientation/x");
  const auto oy = flat->GetColumnByName(prefix + "orientation/y");
  const auto oz = flat->GetColumnByName(prefix + "orientation/z");
  const auto ow = flat->GetColumnByName(prefix + "orientation/w");
  if (!px || !py || !pz || !ox || !oy || !oz || !ow) {
    return PJ::unexpected(std::string("pose topic '") + topic_name + "' missing position/* or orientation/* columns");
  }
  const auto frame_col = flat->GetColumnByName("frame_id");
  const auto ts_col = ts_field.empty() ? nullptr : flat->GetColumnByName(ts_field);

  auto topic_handle = host.registerObjectTopic(source, topic_name, kCanonicalPosesInFrameMetadata);
  if (!topic_handle) {
    return PJ::unexpected(std::move(topic_handle).error());
  }

  ObjectPushOutcome outcome;
  const std::int64_t num_rows = flat->num_rows();
  for (std::int64_t row = 0; row < num_rows; ++row) {
    const std::int64_t ts_ns = ts_col ? arrowI64Opt(ts_col, row).value_or(synth_anchor_ns + row * synth_interval_ns)
                                      : (synth_anchor_ns + row * synth_interval_ns);
    PJ::sdk::Pose pose;
    pose.position =
        PJ::sdk::Vector3{.x = arrowDoubleAt(px, row), .y = arrowDoubleAt(py, row), .z = arrowDoubleAt(pz, row)};
    pose.orientation = sanitizeQuaternion(
        PJ::sdk::Quaternion{
            .x = arrowDoubleAt(ox, row),
            .y = arrowDoubleAt(oy, row),
            .z = arrowDoubleAt(oz, row),
            .w = arrowDoubleAt(ow, row)});

    PJ::sdk::PosesInFrame poses;
    poses.timestamp_ns = ts_ns;
    poses.frame_id = frame_col ? arrowStringAt(frame_col, row) : std::string{};
    poses.poses.push_back(pose);
    const std::vector<std::uint8_t> blob = PJ::serializePosesInFrame(poses);
    auto status = host.pushOwnedObject(*topic_handle, ts_ns, PJ::Span<const std::uint8_t>(blob.data(), blob.size()));
    if (!status) {
      return PJ::unexpected(std::move(status).error());
    }
    teeIfPresent(ctx, ts_ns, blob);
    ++outcome.pushed;
  }
  return outcome;
}

PJ::Expected<ObjectPushOutcome> pushFrameTransformsRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table) {
  const PJ::sdk::ToolboxHostView& host = ctx.host;
  const PJ::sdk::DataSourceHandle source = ctx.source;
  const std::string& topic_name = ctx.topic_name;
  const std::string& ts_field = ctx.ts_field;
  const std::int64_t synth_anchor_ns = ctx.synth_anchor_ns;
  const std::int64_t synth_interval_ns = ctx.synth_interval_ns;
  if (!host.valid()) {
    return PJ::unexpected(std::string("toolbox host not bound"));
  }
  if (!table) {
    return PJ::unexpected(std::string("transform topic '") + topic_name + "': null table");
  }

  auto topic_handle = host.registerObjectTopic(source, topic_name, kCanonicalFrameTransformsMetadata);
  if (!topic_handle) {
    return PJ::unexpected(std::move(topic_handle).error());
  }
  ObjectPushOutcome outcome;

  // `frame_transform`: a `transforms` list<struct> per row → one batch each.
  if (const auto transforms_col = table->GetColumnByName("transforms")) {
    const auto ts_col = ts_field.empty() ? nullptr : table->GetColumnByName(ts_field);
    const std::int64_t num_rows = table->num_rows();
    for (std::int64_t row = 0; row < num_rows; ++row) {
      const std::int64_t ts_ns = ts_col ? arrowI64Opt(ts_col, row).value_or(synth_anchor_ns + row * synth_interval_ns)
                                        : (synth_anchor_ns + row * synth_interval_ns);
      std::vector<PJ::sdk::FrameTransform> transforms = readTransformList(transforms_col, row);
      if (transforms.empty()) {
        ++outcome.skipped;
        if (outcome.first_error.empty()) {
          outcome.first_error =
              std::string("transform topic '") + topic_name + "' row " + std::to_string(row) + ": empty 'transforms'";
        }
        continue;
      }
      for (auto& ft : transforms) {
        ft.timestamp = ts_ns;
      }
      PJ::sdk::FrameTransforms batch;
      batch.transforms = std::move(transforms);
      const std::vector<std::uint8_t> blob = PJ::serializeFrameTransforms(batch);
      auto status = host.pushOwnedObject(*topic_handle, ts_ns, PJ::Span<const std::uint8_t>(blob.data(), blob.size()));
      if (!status) {
        return PJ::unexpected(std::move(status).error());
      }
      teeIfPresent(ctx, ts_ns, blob);
      ++outcome.pushed;
    }
    return outcome;
  }

  // `transform`: one transform per row. Flatten the struct columns and read the
  // translation/rotation children, parent (header/frame_id) + child
  // (target_frame_id) frame ids.
  auto flat_res = flattenStructColumns(table);
  if (!flat_res.ok()) {
    return PJ::unexpected(
        std::string("transform topic '") + topic_name + "': flatten failed: " + flat_res.status().ToString());
  }
  const std::shared_ptr<arrow::Table> flat = *flat_res;
  const auto tx = flat->GetColumnByName("translation/x");
  const auto ty = flat->GetColumnByName("translation/y");
  const auto tz = flat->GetColumnByName("translation/z");
  const auto rx = flat->GetColumnByName("rotation/x");
  const auto ry = flat->GetColumnByName("rotation/y");
  const auto rz = flat->GetColumnByName("rotation/z");
  const auto rw = flat->GetColumnByName("rotation/w");
  if (!tx || !ty || !tz || !rx || !ry || !rz || !rw) {
    return PJ::unexpected(
        std::string("transform topic '") + topic_name + "' missing translation/* or rotation/* columns");
  }
  const auto child_col = flat->GetColumnByName("target_frame_id");
  const auto parent_col = firstPresentColumn(flat, {"header/frame_id", "frame_id"});
  const auto ts_col = ts_field.empty() ? nullptr : flat->GetColumnByName(ts_field);

  const std::int64_t num_rows = flat->num_rows();
  for (std::int64_t row = 0; row < num_rows; ++row) {
    const std::int64_t ts_ns = ts_col ? arrowI64Opt(ts_col, row).value_or(synth_anchor_ns + row * synth_interval_ns)
                                      : (synth_anchor_ns + row * synth_interval_ns);
    PJ::sdk::FrameTransform ft;
    ft.timestamp = ts_ns;
    ft.parent_frame_id = parent_col ? arrowStringAt(parent_col, row) : std::string{};
    ft.child_frame_id = child_col ? arrowStringAt(child_col, row) : std::string{};
    ft.translation =
        PJ::sdk::Vector3{.x = arrowDoubleAt(tx, row), .y = arrowDoubleAt(ty, row), .z = arrowDoubleAt(tz, row)};
    ft.rotation = sanitizeQuaternion(
        PJ::sdk::Quaternion{
            .x = arrowDoubleAt(rx, row),
            .y = arrowDoubleAt(ry, row),
            .z = arrowDoubleAt(rz, row),
            .w = arrowDoubleAt(rw, row)});
    PJ::sdk::FrameTransforms batch;
    batch.transforms.push_back(std::move(ft));
    const std::vector<std::uint8_t> blob = PJ::serializeFrameTransforms(batch);
    auto status = host.pushOwnedObject(*topic_handle, ts_ns, PJ::Span<const std::uint8_t>(blob.data(), blob.size()));
    if (!status) {
      return PJ::unexpected(std::move(status).error());
    }
    teeIfPresent(ctx, ts_ns, blob);
    ++outcome.pushed;
  }
  return outcome;
}

PJ::Expected<ObjectPushOutcome> pushOccupancyGridRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table) {
  const PJ::sdk::ToolboxHostView& host = ctx.host;
  const PJ::sdk::DataSourceHandle source = ctx.source;
  const std::string& topic_name = ctx.topic_name;
  const std::string& ts_field = ctx.ts_field;
  const std::int64_t synth_anchor_ns = ctx.synth_anchor_ns;
  const std::int64_t synth_interval_ns = ctx.synth_interval_ns;
  if (!host.valid()) {
    return PJ::unexpected(std::string("toolbox host not bound"));
  }
  if (!table) {
    return PJ::unexpected(std::string("occupancy_grid topic '") + topic_name + "': null table");
  }
  // info/origin/map_load_time are struct columns; flatten to read the scalar
  // metadata + origin pose. The dense `data` list<int8> passes through flatten.
  auto flat_res = flattenStructColumns(table);
  if (!flat_res.ok()) {
    return PJ::unexpected(
        std::string("occupancy_grid topic '") + topic_name + "': flatten failed: " + flat_res.status().ToString());
  }
  const std::shared_ptr<arrow::Table> flat = *flat_res;
  const auto data_col = flat->GetColumnByName("data");
  if (!data_col) {
    return PJ::unexpected(std::string("occupancy_grid topic '") + topic_name + "' missing 'data' column");
  }
  const auto resolution_col = firstPresentColumn(flat, {"info/resolution", "resolution"});
  const auto width_col = firstPresentColumn(flat, {"info/width", "width"});
  const auto height_col = firstPresentColumn(flat, {"info/height", "height"});
  const auto ox = firstPresentColumn(flat, {"info/origin/position/x", "origin/position/x"});
  const auto oy = firstPresentColumn(flat, {"info/origin/position/y", "origin/position/y"});
  const auto oz = firstPresentColumn(flat, {"info/origin/position/z", "origin/position/z"});
  const auto qx = firstPresentColumn(flat, {"info/origin/orientation/x", "origin/orientation/x"});
  const auto qy = firstPresentColumn(flat, {"info/origin/orientation/y", "origin/orientation/y"});
  const auto qz = firstPresentColumn(flat, {"info/origin/orientation/z", "origin/orientation/z"});
  const auto qw = firstPresentColumn(flat, {"info/origin/orientation/w", "origin/orientation/w"});
  const auto frame_col = firstPresentColumn(flat, {"header/frame_id", "frame_id"});
  const auto ts_col = ts_field.empty() ? nullptr : flat->GetColumnByName(ts_field);

  auto topic_handle = host.registerObjectTopic(source, topic_name, kCanonicalOccupancyGridMetadata);
  if (!topic_handle) {
    return PJ::unexpected(std::move(topic_handle).error());
  }

  ObjectPushOutcome outcome;
  const std::int64_t num_rows = flat->num_rows();
  for (std::int64_t row = 0; row < num_rows; ++row) {
    const std::int64_t ts_ns = ts_col ? arrowI64Opt(ts_col, row).value_or(synth_anchor_ns + row * synth_interval_ns)
                                      : (synth_anchor_ns + row * synth_interval_ns);
    const std::int32_t w = arrowI32At(width_col, row);
    const std::int32_t h = arrowI32At(height_col, row);
    const double resolution = arrowDoubleAt(resolution_col, row);
    const NumericList data = readNumericList(data_col, row);
    const std::int64_t cells = static_cast<std::int64_t>(w) * static_cast<std::int64_t>(h);
    // A zero/negative/non-finite resolution is geometrically meaningless (cells
    // collapse to a point); reject it alongside bad geometry rather than push a
    // degenerate map that masks a server schema mismatch.
    if (w <= 0 || h <= 0 || !std::isfinite(resolution) || resolution <= 0.0 || !data.valid() || data.count < cells) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error = std::string("occupancy_grid topic '") + topic_name + "' row " + std::to_string(row) +
                              ": bad geometry/resolution or short 'data'";
      }
      continue;
    }
    PJ::sdk::OccupancyGrid grid;
    grid.timestamp_ns = ts_ns;
    grid.frame_id = frame_col ? arrowStringAt(frame_col, row) : std::string{};
    grid.resolution = resolution;
    grid.width = static_cast<std::uint32_t>(w);
    grid.height = static_cast<std::uint32_t>(h);
    grid.origin.position =
        PJ::sdk::Vector3{.x = arrowDoubleAt(ox, row), .y = arrowDoubleAt(oy, row), .z = arrowDoubleAt(oz, row)};
    grid.origin.orientation = PJ::sdk::Quaternion{
        .x = arrowDoubleAt(qx, row),
        .y = arrowDoubleAt(qy, row),
        .z = arrowDoubleAt(qz, row),
        .w = qw ? arrowDoubleAt(qw, row) : 1.0};
    // `data` is int8 (−1/0..100); store the raw bytes (codec copies them).
    grid.data = PJ::Span<const std::uint8_t>(data.bytes.data(), static_cast<std::size_t>(cells));
    const std::vector<std::uint8_t> blob = PJ::serializeOccupancyGrid(grid);
    auto status = host.pushOwnedObject(*topic_handle, ts_ns, PJ::Span<const std::uint8_t>(blob.data(), blob.size()));
    if (!status) {
      return PJ::unexpected(std::move(status).error());
    }
    teeIfPresent(ctx, ts_ns, blob);
    ++outcome.pushed;
  }
  return outcome;
}

PJ::Expected<ObjectPushOutcome> pushLaserScanRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table) {
  const PJ::sdk::ToolboxHostView& host = ctx.host;
  const PJ::sdk::DataSourceHandle source = ctx.source;
  const std::string& topic_name = ctx.topic_name;
  const std::string& ts_field = ctx.ts_field;
  const std::int64_t synth_anchor_ns = ctx.synth_anchor_ns;
  const std::int64_t synth_interval_ns = ctx.synth_interval_ns;
  if (!host.valid()) {
    return PJ::unexpected(std::string("toolbox host not bound"));
  }
  if (!table) {
    return PJ::unexpected(std::string("laser_scan topic '") + topic_name + "': null table");
  }
  // Flatten to surface header/frame_id; the `ranges`/`intensities` lists pass through.
  auto flat_res = flattenStructColumns(table);
  if (!flat_res.ok()) {
    return PJ::unexpected(
        std::string("laser_scan topic '") + topic_name + "': flatten failed: " + flat_res.status().ToString());
  }
  const std::shared_ptr<arrow::Table> flat = *flat_res;
  const auto ranges_col = flat->GetColumnByName("ranges");
  if (!ranges_col) {
    return PJ::unexpected(std::string("laser_scan topic '") + topic_name + "' missing 'ranges' column");
  }
  const auto intensities_col = flat->GetColumnByName("intensities");
  const auto angle_min_col = flat->GetColumnByName("angle_min");
  const auto angle_inc_col = flat->GetColumnByName("angle_increment");
  const auto range_min_col = flat->GetColumnByName("range_min");
  const auto range_max_col = flat->GetColumnByName("range_max");
  const auto frame_col = firstPresentColumn(flat, {"header/frame_id", "frame_id"});
  const auto ts_col = ts_field.empty() ? nullptr : flat->GetColumnByName(ts_field);

  auto topic_handle = host.registerObjectTopic(source, topic_name, kCanonicalPointCloudMetadata);
  if (!topic_handle) {
    return PJ::unexpected(std::move(topic_handle).error());
  }

  ObjectPushOutcome outcome;
  const std::int64_t num_rows = flat->num_rows();
  for (std::int64_t row = 0; row < num_rows; ++row) {
    const std::int64_t ts_ns = ts_col ? arrowI64Opt(ts_col, row).value_or(synth_anchor_ns + row * synth_interval_ns)
                                      : (synth_anchor_ns + row * synth_interval_ns);
    const double angle_min = arrowDoubleAt(angle_min_col, row);
    const double angle_inc = arrowDoubleAt(angle_inc_col, row);
    const double range_min = range_min_col ? arrowDoubleAt(range_min_col, row) : 0.0;
    const double range_max =
        range_max_col ? arrowDoubleAt(range_max_col, row) : std::numeric_limits<double>::infinity();
    const NumericList ranges = readNumericList(ranges_col, row);
    if (!ranges.valid()) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error =
            std::string("laser_scan topic '") + topic_name + "' row " + std::to_string(row) + ": empty 'ranges'";
      }
      continue;
    }
    const NumericList intensities = intensities_col ? readNumericList(intensities_col, row) : NumericList{};
    const bool have_intensity = intensities.valid() && intensities.count == ranges.count;

    // Expand the polar scan to packed XYZ (+intensity) points, dropping returns
    // that are non-finite or outside [range_min, range_max].
    std::vector<float> xs;
    std::vector<float> ys;
    std::vector<float> zs;
    std::vector<float> intensity_vals;
    xs.reserve(static_cast<std::size_t>(ranges.count));
    ys.reserve(static_cast<std::size_t>(ranges.count));
    zs.reserve(static_cast<std::size_t>(ranges.count));
    for (std::int64_t i = 0; i < ranges.count; ++i) {
      const double r = numericElemAsDouble(ranges, i);
      if (!std::isfinite(r) || r < range_min || r > range_max) {
        continue;
      }
      const double angle = angle_min + (static_cast<double>(i) * angle_inc);
      xs.push_back(static_cast<float>(r * std::cos(angle)));
      ys.push_back(static_cast<float>(r * std::sin(angle)));
      zs.push_back(0.0F);
      if (have_intensity) {
        intensity_vals.push_back(static_cast<float>(numericElemAsDouble(intensities, i)));
      }
    }
    const std::size_t npoints = xs.size();
    if (npoints == 0) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error =
            std::string("laser_scan topic '") + topic_name + "' row " + std::to_string(row) + ": no valid returns";
      }
      continue;
    }
    const std::uint32_t point_step = have_intensity ? 16U : 12U;
    std::vector<std::uint8_t> buf(npoints * point_step);
    for (std::size_t i = 0; i < npoints; ++i) {
      const std::size_t base = i * point_step;
      std::memcpy(buf.data() + base + 0, &xs[i], sizeof(float));
      std::memcpy(buf.data() + base + 4, &ys[i], sizeof(float));
      std::memcpy(buf.data() + base + 8, &zs[i], sizeof(float));
      if (have_intensity) {
        std::memcpy(buf.data() + base + 12, &intensity_vals[i], sizeof(float));
      }
    }
    PJ::sdk::PointCloud cloud;
    cloud.width = static_cast<std::uint32_t>(npoints);
    cloud.height = 1;
    cloud.point_step = point_step;
    cloud.row_step = point_step * cloud.width;
    cloud.is_dense = true;
    cloud.frame_id = frame_col ? arrowStringAt(frame_col, row) : std::string{};
    cloud.timestamp_ns = ts_ns;
    cloud.fields.push_back(
        PJ::sdk::PointField{.name = "x", .offset = 0, .datatype = PJ::sdk::PointField::Datatype::kFloat32, .count = 1});
    cloud.fields.push_back(
        PJ::sdk::PointField{.name = "y", .offset = 4, .datatype = PJ::sdk::PointField::Datatype::kFloat32, .count = 1});
    cloud.fields.push_back(
        PJ::sdk::PointField{.name = "z", .offset = 8, .datatype = PJ::sdk::PointField::Datatype::kFloat32, .count = 1});
    if (have_intensity) {
      cloud.fields.push_back(
          PJ::sdk::PointField{
              .name = "intensity", .offset = 12, .datatype = PJ::sdk::PointField::Datatype::kFloat32, .count = 1});
    }
    cloud.data = PJ::Span<const std::uint8_t>(buf.data(), buf.size());
    const std::vector<std::uint8_t> blob = PJ::serializePointCloud(cloud);
    auto status = host.pushOwnedObject(*topic_handle, ts_ns, PJ::Span<const std::uint8_t>(blob.data(), blob.size()));
    if (!status) {
      return PJ::unexpected(std::move(status).error());
    }
    teeIfPresent(ctx, ts_ns, blob);
    ++outcome.pushed;
  }
  return outcome;
}

PJ::Expected<ObjectPushOutcome> pushGridCellsRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table) {
  const PJ::sdk::ToolboxHostView& host = ctx.host;
  const PJ::sdk::DataSourceHandle source = ctx.source;
  const std::string& topic_name = ctx.topic_name;
  const std::string& ts_field = ctx.ts_field;
  const std::int64_t synth_anchor_ns = ctx.synth_anchor_ns;
  const std::int64_t synth_interval_ns = ctx.synth_interval_ns;
  if (!host.valid()) {
    return PJ::unexpected(std::string("toolbox host not bound"));
  }
  if (!table) {
    return PJ::unexpected(std::string("grid_cells topic '") + topic_name + "': null table");
  }
  auto flat_res = flattenStructColumns(table);
  if (!flat_res.ok()) {
    return PJ::unexpected(
        std::string("grid_cells topic '") + topic_name + "': flatten failed: " + flat_res.status().ToString());
  }
  const std::shared_ptr<arrow::Table> flat = *flat_res;
  const auto cells_col = flat->GetColumnByName("cells");
  if (!cells_col) {
    return PJ::unexpected(std::string("grid_cells topic '") + topic_name + "' missing 'cells' column");
  }
  const auto cw_col = flat->GetColumnByName("cell_width");
  const auto ch_col = flat->GetColumnByName("cell_height");
  const auto frame_col = firstPresentColumn(flat, {"header/frame_id", "frame_id"});
  const auto ts_col = ts_field.empty() ? nullptr : flat->GetColumnByName(ts_field);

  auto topic_handle = host.registerObjectTopic(source, topic_name, kCanonicalSceneEntitiesMetadata);
  if (!topic_handle) {
    return PJ::unexpected(std::move(topic_handle).error());
  }

  ObjectPushOutcome outcome;
  const std::int64_t num_rows = flat->num_rows();
  for (std::int64_t row = 0; row < num_rows; ++row) {
    const std::int64_t ts_ns = ts_col ? arrowI64Opt(ts_col, row).value_or(synth_anchor_ns + row * synth_interval_ns)
                                      : (synth_anchor_ns + row * synth_interval_ns);
    const double cw = cw_col ? arrowDoubleAt(cw_col, row) : 0.0;
    const double ch = ch_col ? arrowDoubleAt(ch_col, row) : 0.0;
    const std::vector<std::array<double, 3>> cells = readXyzStructList(cells_col, row);
    if (cells.empty()) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error =
            std::string("grid_cells topic '") + topic_name + "' row " + std::to_string(row) + ": empty 'cells'";
      }
      continue;
    }
    PJ::sdk::SceneEntities scene;
    PJ::sdk::SceneEntity entity;
    entity.timestamp = ts_ns;
    entity.frame_id = frame_col ? arrowStringAt(frame_col, row) : std::string{};
    entity.cubes.reserve(cells.size());
    constexpr double kThickness = 0.01;  // sparse cells render as thin flat boxes
    for (const auto& c : cells) {
      PJ::sdk::CubePrimitive cube;
      cube.pose.position = PJ::sdk::Vector3{.x = c[0], .y = c[1], .z = c[2]};
      cube.size = PJ::sdk::Vector3{.x = cw > 0.0 ? cw : 0.0, .y = ch > 0.0 ? ch : 0.0, .z = kThickness};
      cube.color = PJ::sdk::ColorRGBA{.r = 100, .g = 149, .b = 237, .a = 200};  // cornflower, semi-opaque
      entity.cubes.push_back(cube);
    }
    scene.entities.push_back(std::move(entity));
    const std::vector<std::uint8_t> blob = PJ::serializeSceneEntities(scene);
    auto status = host.pushOwnedObject(*topic_handle, ts_ns, PJ::Span<const std::uint8_t>(blob.data(), blob.size()));
    if (!status) {
      return PJ::unexpected(std::move(status).error());
    }
    teeIfPresent(ctx, ts_ns, blob);
    ++outcome.pushed;
  }
  return outcome;
}

PJ::Expected<ObjectPushOutcome> pushColumnarPointCloudRowsToHost(
    const ObjectIngestContext& ctx, const std::shared_ptr<arrow::Table>& table) {
  const PJ::sdk::ToolboxHostView& host = ctx.host;
  const PJ::sdk::DataSourceHandle source = ctx.source;
  const std::string& topic_name = ctx.topic_name;
  const std::string& ts_field = ctx.ts_field;
  const std::int64_t synth_anchor_ns = ctx.synth_anchor_ns;
  const std::int64_t synth_interval_ns = ctx.synth_interval_ns;
  if (!host.valid()) {
    return PJ::unexpected(std::string("toolbox host not bound"));
  }
  if (!table) {
    return PJ::unexpected(std::string("futures point cloud topic '") + topic_name + "': null table");
  }
  // Flatten to surface header/frame_id; the per-point list columns pass through.
  auto flat_res = flattenStructColumns(table);
  if (!flat_res.ok()) {
    return PJ::unexpected(
        std::string("futures point cloud topic '") + topic_name + "': flatten failed: " + flat_res.status().ToString());
  }
  const std::shared_ptr<arrow::Table> flat = *flat_res;
  const auto x_col = flat->GetColumnByName("x");
  const auto y_col = flat->GetColumnByName("y");
  const auto z_col = flat->GetColumnByName("z");
  if (!x_col || !y_col || !z_col) {
    return PJ::unexpected(std::string("futures point cloud topic '") + topic_name + "' missing x/y/z columns");
  }
  const auto frame_col = firstPresentColumn(flat, {"header/frame_id", "frame_id"});
  const auto ts_col = ts_field.empty() ? nullptr : flat->GetColumnByName(ts_field);

  // Recognized optional per-point attributes across lidar/radar/rgbd/tof/stereo.
  static constexpr std::array<std::string_view, 25> kOptionalAttrs = {
      "intensity",
      "reflectivity",
      "beam_id",
      "range",
      "near_ir",
      "azimuth",
      "elevation",
      "confidence",
      "return_type",
      "point_timestamp",
      "rcs",
      "snr",
      "doppler_velocity",
      "vx",
      "vy",
      "vx_comp",
      "vy_comp",
      "ax",
      "ay",
      "radial_speed",
      "rgb",
      "noise",
      "grayscale",
      "luma",
      "cost"};

  auto topic_handle = host.registerObjectTopic(source, topic_name, kCanonicalPointCloudMetadata);
  if (!topic_handle) {
    return PJ::unexpected(std::move(topic_handle).error());
  }

  ObjectPushOutcome outcome;
  const std::int64_t num_rows = flat->num_rows();
  for (std::int64_t row = 0; row < num_rows; ++row) {
    const std::int64_t ts_ns = ts_col ? arrowI64Opt(ts_col, row).value_or(synth_anchor_ns + row * synth_interval_ns)
                                      : (synth_anchor_ns + row * synth_interval_ns);
    // Reserve so the NumericList storage never reallocates — PackAttr holds
    // pointers into it.
    std::vector<NumericList> lists;
    lists.reserve(3 + kOptionalAttrs.size());
    std::vector<PackAttr> attrs;
    lists.push_back(readNumericList(x_col, row));
    lists.push_back(readNumericList(y_col, row));
    lists.push_back(readNumericList(z_col, row));
    if (!lists[0].valid() || !lists[1].valid() || !lists[2].valid()) {
      ++outcome.skipped;
      if (outcome.first_error.empty()) {
        outcome.first_error = std::string("futures point cloud topic '") + topic_name + "' row " + std::to_string(row) +
                              ": missing/empty x/y/z";
      }
      continue;
    }
    const std::int64_t npoints = std::min({lists[0].count, lists[1].count, lists[2].count});
    if (npoints <= 0) {
      ++outcome.skipped;
      continue;
    }
    attrs.push_back(PackAttr{.name = "x", .src = &lists[0]});
    attrs.push_back(PackAttr{.name = "y", .src = &lists[1]});
    attrs.push_back(PackAttr{.name = "z", .src = &lists[2]});
    for (const std::string_view nm : kOptionalAttrs) {
      const auto col = flat->GetColumnByName(std::string(nm));
      if (!col) {
        continue;
      }
      NumericList nl = readNumericList(col, row);
      if (nl.valid() && nl.count >= npoints) {
        lists.push_back(std::move(nl));
        attrs.push_back(PackAttr{.name = std::string(nm), .src = &lists.back()});
      }
    }
    PJ::sdk::PointCloud cloud;
    const std::vector<std::uint8_t> buf = packPointCloud(cloud, attrs, npoints);
    cloud.is_dense = true;
    cloud.frame_id = frame_col ? arrowStringAt(frame_col, row) : std::string{};
    cloud.timestamp_ns = ts_ns;
    cloud.data = PJ::Span<const std::uint8_t>(buf.data(), buf.size());
    const std::vector<std::uint8_t> blob = PJ::serializePointCloud(cloud);
    auto status = host.pushOwnedObject(*topic_handle, ts_ns, PJ::Span<const std::uint8_t>(blob.data(), blob.size()));
    if (!status) {
      return PJ::unexpected(std::move(status).error());
    }
    teeIfPresent(ctx, ts_ns, blob);
    ++outcome.pushed;
  }
  return outcome;
}

}  // namespace mosaico
