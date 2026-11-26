// SPDX-License-Identifier: MIT
#pragma once

#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>
#include <string>

#include "cloud_common.hpp"

namespace pj3d {

/// Parse a whole .pcd file (header + body) into a packed point cloud stamped with `frame_id`. Errors on malformed
/// headers, unsupported datatypes (e.g. 8-byte ints) or size mismatch.
[[nodiscard]] PJ::Expected<PJ::sdk::PointCloud> readPcd(PJ::Span<const uint8_t> file_bytes, std::string frame_id);

}  // namespace pj3d
