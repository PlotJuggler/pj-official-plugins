// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <pj_base/builtin/mesh3d.hpp>
#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>
#include <string>

#include "cloud_common.hpp"

namespace pj3d {

/// Result of reading a .ply: exactly one of `cloud` / `mesh` is populated.
/// Both are self-owning (the cloud via its PointCloud `anchor`, the mesh via its
/// Mesh3D `anchor`), so a PlyResult is safe to move/store independent of the
/// input buffer. `num_faces` is 0 for clouds.
struct PlyResult {
  std::optional<PJ::sdk::PointCloud> cloud;
  std::optional<PJ::sdk::Mesh3D> mesh;
  uint64_t num_faces = 0;
};

/// Parse a whole .ply file (ascii header + ascii/binary body). Vertices-only
/// files (no `face` element with entries) become a packed PointCloud via
/// cloud_common; files with faces become a self-owning Mesh3D holding the
/// raw .ply bytes (the renderer/Assimp parses the geometry, not us).
/// `frame_id` is stamped onto whichever result is populated. Returns a
/// PJ::Expected error (not an exception) on malformed input: a missing `ply`
/// magic line, malformed headers, or unsupported vertex property types.
[[nodiscard]] PJ::Expected<PlyResult> readPly(PJ::Span<const uint8_t> file_bytes, std::string frame_id);

}  // namespace pj3d
