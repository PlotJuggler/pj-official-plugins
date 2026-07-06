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

/// Hard cap on the total packed payload size (width*height*point_step), in
/// bytes. Guards against overflow in the point-count/stride arithmetic and
/// against pathological allocations; also keeps row_step (point_step*width)
/// representable in a uint32_t.
inline constexpr uint64_t kMaxCloudBytes = 1ull << 31;  // 2 GiB

/// Assign a byte offset to each field (tight packing, source order) and return
/// the total per-point stride. Errors if any datatype is kUnknown or if the
/// accumulated stride would not fit in the uint32_t point_step.
[[nodiscard]] PJ::Expected<FieldLayout> computeLayout(const std::vector<ParsedField>& fields);

/// Build a packed little-endian PointCloud from a body. The returned
/// PointCloud owns its packed bytes via `anchor` (a shared_ptr to the backing
/// vector), so it is safe to copy/move/store past the call.
/// - kBinaryLittleEndian: `body` is num_points*point_step bytes, same layout as
///   the output; copied directly.
/// - kBinaryBigEndian: `body` is num_points*point_step bytes; each element is
///   byte-swapped to little-endian.
/// - kAscii: `body` is whitespace-separated numeric tokens, width*height rows of
///   (sum of field counts) values each.
/// `width*height` is the point count. `is_bigendian` on the result is always
/// false. Errors on size mismatch, malformed ascii, or if the total payload
/// would exceed `kMaxCloudBytes`.
[[nodiscard]] PJ::Expected<PJ::sdk::PointCloud> buildPointCloud(
    const std::vector<ParsedField>& fields, uint32_t width, uint32_t height, bool is_dense, std::string frame_id,
    DataFormat format, PJ::Span<const uint8_t> body);

/// Mean of the x/y/z fields over all points. nullopt if x, y, or z is absent,
/// or the cloud is empty (width*height == 0). Returns {cx, cy, cz}.
[[nodiscard]] std::optional<std::array<double, 3>> computeCentroid(const PJ::sdk::PointCloud& cloud);

}  // namespace pj3d
