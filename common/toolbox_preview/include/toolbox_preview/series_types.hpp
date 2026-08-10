// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0
//
// series_types — the type vocabulary shared by generator-toolbox previews: which
// primitive types may be a source curve at all, and how to widen a raw Arrow value
// column of any of them into the doubles the preview (and the marker engine) work in.
//
// Deliberately split out of series_catalog so both halves are unit-testable without a
// host: `decodeSeriesAsDouble` takes plain pointers rather than an ArrowArray, so a test
// can hand it a stack array instead of standing up a fake toolbox host.

#pragma once

#include <cstddef>
#include <cstdint>
#include <pj_base/type_tree.hpp>  // PJ::PrimitiveType
#include <vector>

namespace toolbox_preview {

/// May a series of this primitive type be a preview/detection-rule source?
///
/// This MIRRORS the host's own gate, `PJ::isPlottablePrimitive` in
/// pj_runtime/include/pj_runtime/CatalogModel.h, which is the authority. It cannot be
/// included: pj_runtime is the application, not the plugin SDK. Keep the two in step.
///
/// This is not cosmetic filtering. The host's marker-series resolver rejects
/// non-plottable fields outright (MainWindow.cpp, markerSeriesField), so a rule aimed at
/// one receives a nil series and dies with "attempt to index nil value". A source list
/// that offers them is offering a guaranteed failure.
///
/// Exhaustive switch with NO default: a new PrimitiveType must break the build here
/// instead of being silently classified.
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
    // Bool IS plottable here, unlike toolbox_fft's isNumericType, which excludes it on
    // purpose (an FFT over a flag is meaningless). The "Boolean flag (edges)" builtin in
    // toolbox_anomaly_detector/core/anomaly_helpers.cpp reads bool series, so dropping
    // bool would silently remove that whole detector from the UI.
    //
    // Two sibling predicates exist and are deliberately NOT the same question:
    // toolbox_fft/fft_plugin.cpp (isNumericType, excludes bool) and
    // toolbox_data_exporter/data_exporter.cpp (isNumericType, same set as this one but
    // asking "is this an exportable numeric column"). Keep them in step when the set of
    // PrimitiveTypes changes — the exhaustive switch will point at each one.
    case PJ::PrimitiveType::kBool:
      return true;
    case PJ::PrimitiveType::kString:
    case PJ::PrimitiveType::kUnspecified:
      return false;
  }
  return false;
}

/// Widen one Arrow value column to doubles, pairing each sample with its timestamp.
///
/// Why this exists at all: it fills a gap in the SDK, and should move there eventually.
/// `MaterializedSeriesView` only generates `valuesAs*()` for the ten NUMERIC types — there
/// is no `valuesAsBool()` (Arrow BOOL is a packed bitmap, so it cannot come from that
/// macro, though it could exist as a method), and it exposes neither the validity bitmap
/// nor the raw ArrowArray. So every `valuesAs*()` caller in this repo silently reads nulls
/// as zeros; this decoder is one consumer opting out of that via `readSeriesArrow`.
/// The deep fix is `valuesAsBool()` + `validity()` in pj_base, which would delete this
/// file — it needs an SDK release and a core-version bump, hence the local copy for now.
///
/// @param type        primitive type of the value column (from the catalog).
/// @param values      ArrowArray::buffers[1] of the value column.
/// @param validity    ArrowArray::buffers[0], or nullptr when the host omitted it
///                    (null_count == 0) — then every sample is valid.
/// @param offset      ArrowArray::offset. For kBool this is a BIT offset, not a byte one.
/// @param timestamps  int64 nanoseconds, already offset-adjusted by the caller.
/// @param count       number of samples to read.
///
/// Null samples are DROPPED along with their timestamp rather than read as zero. That is
/// required, not tidy: a null bool leaves its bit at 0 (the host's ArrowArrayAppendNull),
/// and "Boolean flag (edges)" detects edges with `v ~= 0`, so a sparse flag field would
/// sprout a phantom falling and rising edge at every gap.
///
/// Returns false (leaving the outputs untouched) for a type with no numeric reading —
/// kString / kUnspecified — or for null input pointers.
[[nodiscard]] bool decodeSeriesAsDouble(
    PJ::PrimitiveType type, const void* values, const std::uint8_t* validity, std::int64_t offset,
    const std::int64_t* timestamps, std::size_t count, std::vector<double>& out_timestamps,
    std::vector<double>& out_values);

}  // namespace toolbox_preview
