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

/// One field descriptor parsed from a PLY/PCD header, before offsets are known.
struct ParsedField {
  std::string name;
  Datatype datatype = Datatype::kUnknown;
  uint32_t count = 1;  ///< elements per point (PCD COUNT; always 1 for PLY).
};

/// Field list with byte offsets resolved, plus the per-point stride.
struct FieldLayout {
  std::vector<PJ::sdk::PointField> fields;  ///< offsets filled in.
  uint32_t point_step = 0;
};

/// A decoded cloud that owns its packed payload. `cloud.data` views `storage`,
/// so keep this struct alive until the payload is serialized/copied.
struct BuiltCloud {
  std::vector<uint8_t> storage;
  PJ::sdk::PointCloud cloud;
};

/// Assign a byte offset to each field (tight packing, source order) and return
/// the total per-point stride. Errors if any datatype is kUnknown.
[[nodiscard]] PJ::Expected<FieldLayout> computeLayout(const std::vector<ParsedField>& fields);

/// Build a packed little-endian PointCloud from a body.
/// - kBinaryLittleEndian: `body` is num_points*point_step bytes, same layout as
///   the output; copied directly.
/// - kBinaryBigEndian: `body` is num_points*point_step bytes; each element is
///   byte-swapped to little-endian.
/// - kAscii: `body` is whitespace-separated numeric tokens, width*height rows of
///   (sum of field counts) values each.
/// `width*height` is the point count. `is_bigendian` on the result is always
/// false. Errors on size mismatch or malformed ascii.
[[nodiscard]] PJ::Expected<BuiltCloud> buildPointCloud(
    const std::vector<ParsedField>& fields, uint32_t width, uint32_t height, bool is_dense, std::string frame_id,
    DataFormat format, PJ::Span<const uint8_t> body);

/// Mean of the x/y/z fields over all points. nullopt if x, y, or z is absent or
/// non-float. Returns {cx, cy, cz}.
[[nodiscard]] std::optional<std::array<double, 3>> computeCentroid(const PJ::sdk::PointCloud& cloud);

}  // namespace pj3d
