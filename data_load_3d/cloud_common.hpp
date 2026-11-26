// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <pj_base/builtin/point_cloud.hpp>
#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>
#include <string>
#include <vector>

namespace pj3d {

using Datatype = PJ::sdk::PointField::Datatype;

/// How a point body is encoded in the source file.
enum class DataFormat { kAscii, kBinaryLittleEndian, kBinaryBigEndian };

/// A PLY/PCD header field, before offsets are known.
struct ParsedField {
  std::string name;
  Datatype datatype = Datatype::kUnknown;
  uint32_t count = 1;  ///< elements per point (PCD COUNT; always 1 for PLY).
};

struct FieldLayout {
  std::vector<PJ::sdk::PointField> fields;  ///< offsets filled in.
  uint32_t point_step = 0;
};

/// Cap on the packed payload (width*height*point_step) in bytes: guards the point-count/stride arithmetic and
/// pathological allocations, and keeps row_step (point_step*width) representable in a uint32_t.
inline constexpr uint64_t kMaxCloudBytes = 1ull << 31;  // 2 GiB

/// Tight packing in source order. Errors if any datatype is kUnknown or the stride overflows uint32_t.
[[nodiscard]] PJ::Expected<FieldLayout> computeLayout(const std::vector<ParsedField>& fields);

/// Build a packed little-endian PointCloud (owning its bytes via `anchor`) from a body; `width*height` is the
/// point count and `is_bigendian` on the result is always false.
/// - kBinaryLittleEndian: `body` is num_points*point_step bytes in the output layout; copied directly.
/// - kBinaryBigEndian: same size; each element is byte-swapped to little-endian.
/// - kAscii: whitespace-separated numeric tokens, width*height rows of (sum of field counts) values.
/// Errors on size mismatch, malformed ascii, or a payload over `kMaxCloudBytes`.
[[nodiscard]] PJ::Expected<PJ::sdk::PointCloud> buildPointCloud(
    const std::vector<ParsedField>& fields, uint32_t width, uint32_t height, bool is_dense, std::string frame_id,
    DataFormat format, PJ::Span<const uint8_t> body);

/// Mean {cx, cy, cz} of the x/y/z fields; nullopt if one is absent or the cloud is empty.
[[nodiscard]] std::optional<std::array<double, 3>> computeCentroid(const PJ::sdk::PointCloud& cloud);

}  // namespace pj3d
