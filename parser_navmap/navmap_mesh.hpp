/**
 * NavMap / NavMapLayer messages -> sdk::MeshMap / sdk::MeshMapUpdate.
 *
 * Layers pass through as MeshMap channels in their wire type; only the geometry
 * is repacked (NavMap keeps one array per axis and per corner, MeshMap wants
 * them interleaved). Styling is left to the 3D view.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <pj_base/builtin/mesh_map.hpp>
#include <pj_base/builtin/mesh_map_update.hpp>
#include <pj_base/expected.hpp>
#include <string>
#include <vector>

#include "navmap_msg_decoder.hpp"

namespace navmap_parser {

/// The most memory the sparse layers of one map may expand to. A sparse layer
/// lists a few NavCels but is stored as one element per NavCel, so a small
/// message with many of them could otherwise demand gigabytes.
inline constexpr size_t kMaxSparseLayerBytes = size_t{1} << 30;

struct NavMapLayer {
  std::string frame_id;  ///< Header frame; set only for a standalone NavMapLayer message.
  std::string name;
  std::string unit;
  bool has_unknown = false;
  double unknown_value = std::numeric_limits<double>::quiet_NaN();
  bool has_geometry_id = false;
  uint64_t geometry_id = 0;
  /// Empty: `values` covers every NavCel. Otherwise values[k] belongs to NavCel indices[k].
  std::vector<uint32_t> indices;
  /// The data array the type tag names (or, when that one is empty, the one
  /// that is filled), in its wire type, which the channel datatype follows.
  NumberArray values = std::vector<float>{};
};

struct NavMapSurface {
  std::string frame_id;
  std::vector<uint32_t> navcels;
};

/// Connects two non-adjacent NavCels (stairs, elevator, ...).
struct NavMapLink {
  uint32_t from_navcel = 0;
  uint32_t to_navcel = 0;
  uint8_t type = 0;
  bool bidirectional = false;
  double length = std::numeric_limits<double>::quiet_NaN();  ///< travel length (m); NaN when the layout has none
};

/// A NavMap message after validation: every index is in range and every kept
/// layer matches the NavCel count.
struct NavMapData {
  std::string frame_id;
  std::vector<float> x, y, z;
  bool has_vertex_rgba = false;  ///< true only when r/g/b match the vertex count
  std::vector<uint8_t> r, g, b, a;
  std::vector<uint32_t> v0, v1, v2;
  std::vector<NavMapSurface> surfaces;
  std::vector<NavMapLayer> layers;
  bool has_geometry_id = false;
  uint64_t geometry_id = 0;
  std::vector<NavMapLink> links;
  /// Layers dropped: data that does not match the NavCel count, an index out
  /// of range, a name that cannot be a channel (empty, reserved, repeated), a
  /// sparse integer layer with no no-data value to fill the unlisted NavCels
  /// with, 64-bit integer data, or a sparse layer past `max_sparse_bytes`.
  uint32_t dropped_layers = 0;
  /// Links dropped: a NavCel out of range or a field that is not a valid integer.
  uint32_t dropped_links = 0;
};

/// Validates a decoded NavMap and moves its arrays out of @p root. Fields
/// missing from older message revisions keep their defaults. An empty header
/// frame falls back to the first surface's frame. Invalid layers and links are
/// dropped and counted rather than failing the map. The sparse layers are kept
/// in order while the memory they expand to (NavCel count times element size
/// each) stays within @p max_sparse_bytes; dense layers are bounded by the payload.
[[nodiscard]] PJ::Expected<NavMapData> extractNavMap(Node& root, size_t max_sparse_bytes = kMaxSparseLayerBytes);

/// Accepts a standalone NavMapLayer message or an element of NavMap.layers.
[[nodiscard]] NavMapLayer extractLayer(Node& node);

/// The header stamp of a decoded message in nanoseconds; nullopt when it is
/// missing, malformed or not positive (an unset stamp).
[[nodiscard]] std::optional<int64_t> positiveHeaderStampNs(const Node& root);

/// See README.md "Conversion" for the channel layout. `frame_id` is the header
/// frame: MeshMap has one frame per object, and NavMap surfaces share the map
/// frame in practice.
[[nodiscard]] PJ::sdk::MeshMap buildMeshMap(NavMapData map, int64_t timestamp_ns);

/// One face patch for the layer's channel (dense without indices, sparse
/// otherwise) with the name, datatype and info a map gives the same layer.
/// Fails when the data does not match the listed indices or the name is one a
/// map drops.
[[nodiscard]] PJ::Expected<PJ::sdk::MeshMapUpdate> buildMeshMapUpdate(NavMapLayer layer, int64_t timestamp_ns);

}  // namespace navmap_parser
