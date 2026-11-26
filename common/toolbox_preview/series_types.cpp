// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

#include "toolbox_preview/series_types.hpp"

namespace toolbox_preview {
namespace {

/// A null bitmap means "no nulls": the host omits buffers[0] entirely when null_count is 0.
[[nodiscard]] bool isValidAt(const std::uint8_t* validity, std::int64_t index) noexcept {
  if (validity == nullptr) {
    return true;
  }
  const auto byte = static_cast<std::size_t>(index / 8);
  const auto bit = static_cast<unsigned>(index % 8);
  return ((validity[byte] >> bit) & 1U) != 0U;
}

/// The ten numeric columns: a contiguous array of the matching C type. `offset` indexes the value
/// buffer; `timestamps` already points at sample 0.
template <typename T>
void appendNumeric(
    const void* values, const std::uint8_t* validity, std::int64_t offset, const std::int64_t* timestamps,
    std::size_t count, std::vector<double>& out_timestamps, std::vector<double>& out_values) {
  const auto* typed = static_cast<const T*>(values);
  for (std::size_t i = 0; i < count; ++i) {
    const std::int64_t index = offset + static_cast<std::int64_t>(i);
    if (!isValidAt(validity, index)) {
      continue;
    }
    out_timestamps.push_back(static_cast<double>(timestamps[i]));
    out_values.push_back(static_cast<double>(typed[static_cast<std::size_t>(index)]));
  }
}

/// Arrow BOOL is a PACKED bitmap, one bit per sample, not an array of `bool` (reading it as
/// `static_cast<const bool*>` silently yields garbage). `offset` is therefore a bit offset.
void appendBool(
    const void* values, const std::uint8_t* validity, std::int64_t offset, const std::int64_t* timestamps,
    std::size_t count, std::vector<double>& out_timestamps, std::vector<double>& out_values) {
  const auto* bits = static_cast<const std::uint8_t*>(values);
  for (std::size_t i = 0; i < count; ++i) {
    const std::int64_t index = offset + static_cast<std::int64_t>(i);
    if (!isValidAt(validity, index)) {
      continue;
    }
    const auto byte = static_cast<std::size_t>(index / 8);
    const auto bit = static_cast<unsigned>(index % 8);
    out_timestamps.push_back(static_cast<double>(timestamps[i]));
    out_values.push_back(((bits[byte] >> bit) & 1U) != 0U ? 1.0 : 0.0);
  }
}

}  // namespace

bool decodeSeriesAsDouble(
    PJ::PrimitiveType type, const void* values, const std::uint8_t* validity, std::int64_t offset,
    const std::int64_t* timestamps, std::size_t count, std::vector<double>& out_timestamps,
    std::vector<double>& out_values) {
  if (values == nullptr || timestamps == nullptr) {
    return false;
  }
  out_timestamps.reserve(count);
  out_values.reserve(count);

  // One place for the argument list, so a signature change cannot miss some call sites.
  const auto numeric = [&](auto sample) {
    appendNumeric<decltype(sample)>(values, validity, offset, timestamps, count, out_timestamps, out_values);
    return true;
  };

  // Exhaustive switch with no default, as in isPlottableType.
  switch (type) {
    case PJ::PrimitiveType::kFloat64:
      return numeric(double{});
    case PJ::PrimitiveType::kFloat32:
      return numeric(float{});
    case PJ::PrimitiveType::kInt8:
      return numeric(std::int8_t{});
    case PJ::PrimitiveType::kInt16:
      return numeric(std::int16_t{});
    case PJ::PrimitiveType::kInt32:
      return numeric(std::int32_t{});
    case PJ::PrimitiveType::kInt64:
      return numeric(std::int64_t{});
    case PJ::PrimitiveType::kUint8:
      return numeric(std::uint8_t{});
    case PJ::PrimitiveType::kUint16:
      return numeric(std::uint16_t{});
    case PJ::PrimitiveType::kUint32:
      return numeric(std::uint32_t{});
    case PJ::PrimitiveType::kUint64:
      return numeric(std::uint64_t{});
    case PJ::PrimitiveType::kBool:
      appendBool(values, validity, offset, timestamps, count, out_timestamps, out_values);
      return true;
    case PJ::PrimitiveType::kString:
    case PJ::PrimitiveType::kUnspecified:
      // Unreachable through SeriesCatalog (isPlottableType filters these first); the contract is
      // "returns false, writes nothing".
      return false;
  }
  return false;
}

}  // namespace toolbox_preview
