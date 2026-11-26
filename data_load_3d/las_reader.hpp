// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <pj_base/builtin/point_cloud.hpp>
#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>
#include <string>

namespace pj3d {

struct LasResult {
  PJ::sdk::PointCloud cloud;
  /// Added back to the cloud's x/y/z to recover file coordinates. Per axis: 0, or the floor of the axis' midpoint
  /// when its coordinates exceed 1e4 in magnitude.
  std::array<double, 3> origin{0.0, 0.0, 0.0};
};

/// Parse an uncompressed ASPRS LAS 1.0-1.4 file (point data record formats 0-10)
/// into a packed little-endian point cloud stamped with `frame_id`. Output fields:
/// x/y/z (float32, scaled and offset per the header, minus `origin`), intensity,
/// return_number, number_of_returns, classification, classification_flags
/// (bit0 synthetic, bit1 key-point, bit2 withheld, bit3 overlap), scan_angle
/// (float32 degrees), user_data, point_source_id, and when the format has them
/// gps_time (float64), red/green/blue (uint8) and nir (uint16).
/// Extra bytes past the format's standard record are skipped; VLRs, CRS and
/// waveform packets are ignored. Errors on LAZ-compressed data, malformed or
/// truncated input, and clouds over kMaxCloudBytes.
[[nodiscard]] PJ::Expected<LasResult> readLas(PJ::Span<const uint8_t> file_bytes, std::string frame_id);

}  // namespace pj3d
