// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// The type vocabulary shared by generator-toolbox previews: which primitive types may be a source
// curve, and how to widen a raw Arrow value column of any of them into the doubles the preview
// (and the marker engine) work in. `decodeSeriesAsDouble` takes plain pointers rather than an
// ArrowArray so tests need no fake toolbox host.

#pragma once

#include <cstddef>
#include <cstdint>
#include <pj_base/type_tree.hpp>  // PJ::PrimitiveType
#include <vector>

namespace toolbox_preview {

/// May a series of this primitive type be a preview/detection-rule source?
///
/// MIRRORS the host's gate, `PJ::isPlottablePrimitive` in pj_runtime/include/pj_runtime/CatalogModel.h
/// (the authority; it cannot be included, pj_runtime being the application). Keep the two in
/// step. The host's marker-series resolver rejects non-plottable fields (MainWindow.cpp,
/// markerSeriesField), so a rule aimed at one receives a nil series and dies with "attempt to
/// index nil value": offering them as sources offers a guaranteed failure.
///
/// Exhaustive switch with NO default: a new PrimitiveType must break the build here instead of
/// being silently classified.
[[nodiscard]] constexpr bool isPlottableType(PJ::PrimitiveType type) noexcept {
  switch (type) {
    case PJ::PrimitiveType::kFloat32:
    case PJ::PrimitiveType::kFloat64:
    case PJ::PrimitiveType::kInt8:
    case PJ::PrimitiveType::kInt16:
    case PJ::PrimitiveType::kInt32:
    case PJ::PrimitiveType::kInt64:
    case PJ::PrimitiveType::kUint8:
    case PJ::PrimitiveType::kUint16:
    case PJ::PrimitiveType::kUint32:
    case PJ::PrimitiveType::kUint64:
    // Bool IS plottable here, unlike toolbox_fft's isNumericType, which excludes it on purpose
    // (an FFT over a flag is meaningless). The "Boolean flag (edges)" builtin in
    // toolbox_anomaly_detector/core/anomaly_helpers.cpp reads bool series, so dropping bool would
    // silently remove that detector from the UI.
    //
    // Two sibling predicates answer different questions: toolbox_fft/fft_plugin.cpp
    // (isNumericType, excludes bool) and toolbox_data_exporter/data_exporter.cpp (isNumericType,
    // same set as this one, asking "is this an exportable numeric column"). Keep them in step when
    // the set of PrimitiveTypes changes.
    case PJ::PrimitiveType::kBool:
      return true;
    case PJ::PrimitiveType::kString:
    case PJ::PrimitiveType::kUnspecified:
      return false;
  }
  return false;
}

/// Widens one Arrow value column to doubles, pairing each sample with its timestamp.
///
/// Fills an SDK gap: `MaterializedSeriesView` only generates `valuesAs*()` for the ten NUMERIC
/// types (no `valuesAsBool()`: Arrow BOOL is a packed bitmap), and exposes neither the validity
/// bitmap nor the raw ArrowArray, so every `valuesAs*()` caller silently reads nulls as zeros.
/// This decoder opts out via `readSeriesArrow`. `valuesAsBool()` + `validity()` in pj_base would
/// make it unnecessary.
///
/// @param type        primitive type of the value column (from the catalog).
/// @param values      ArrowArray::buffers[1] of the value column.
/// @param validity    ArrowArray::buffers[0], or nullptr when the host omitted it
///                    (null_count == 0) — then every sample is valid.
/// @param offset      ArrowArray::offset. For kBool this is a BIT offset, not a byte one.
/// @param timestamps  int64 nanoseconds, already offset-adjusted by the caller.
/// @param count       number of samples to read.
///
/// Null samples are DROPPED along with their timestamp rather than read as zero: a null bool
/// leaves its bit at 0 (the host's ArrowArrayAppendNull), and "Boolean flag (edges)" detects edges
/// with `v ~= 0`, so a sparse flag field would sprout a phantom falling and rising edge at every
/// gap.
///
/// Returns false (leaving the outputs untouched) for a type with no numeric reading (kString /
/// kUnspecified) or for null input pointers.
[[nodiscard]] bool decodeSeriesAsDouble(
    PJ::PrimitiveType type, const void* values, const std::uint8_t* validity, std::int64_t offset,
    const std::int64_t* timestamps, std::size_t count, std::vector<double>& out_timestamps,
    std::vector<double>& out_values);

}  // namespace toolbox_preview
