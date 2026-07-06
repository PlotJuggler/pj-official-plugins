// SPDX-License-Identifier: MIT
#pragma once

#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>
#include <string>

#include "cloud_common.hpp"

namespace pj3d {

/// Parse a whole .pcd file (header + body) into a packed point cloud.
/// `frame_id` is stamped onto the result. Errors on malformed headers,
/// unsupported datatypes (e.g. 8-byte ints), size mismatch, or
/// `binary_compressed` (until Task 6 wires liblzf).
[[nodiscard]] PJ::Expected<PJ::sdk::PointCloud> readPcd(PJ::Span<const uint8_t> file_bytes, std::string frame_id);

}  // namespace pj3d
