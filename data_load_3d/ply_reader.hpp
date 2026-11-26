// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <pj_base/builtin/mesh3d.hpp>
#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>
#include <string>

#include "cloud_common.hpp"

namespace pj3d {

/// Exactly one of `cloud` / `mesh` is populated. Both own their bytes via `anchor`, so a PlyResult outlives the
/// input buffer. `num_faces` is 0 for clouds.
struct PlyResult {
  std::optional<PJ::sdk::PointCloud> cloud;
  std::optional<PJ::sdk::Mesh3D> mesh;
  uint64_t num_faces = 0;
};

/// Vertices-only files (no `face` element with entries) become a packed PointCloud; files with faces become a
/// Mesh3D holding the raw .ply bytes (the renderer/Assimp parses the geometry). Malformed input (missing `ply`
/// magic line, bad headers, unsupported vertex property types) returns an error, never throws.
[[nodiscard]] PJ::Expected<PlyResult> readPly(PJ::Span<const uint8_t> file_bytes, std::string frame_id);

}  // namespace pj3d
