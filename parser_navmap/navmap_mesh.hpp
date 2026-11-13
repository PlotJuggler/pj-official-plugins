/**
 * @file navmap_mesh.hpp
 * @brief NavMap / NavMapLayer messages -> sdk::MeshMap / sdk::MeshMapUpdate.
 *
 * The NavMap arrays arrive one array per coordinate and per corner; MeshMap
 * wants interleaved vertex and face records, so the conversion transcodes once
 * here, as grid_map does for GridMap. Styling is left to the 3D view.
 */
#pragma once

#include <cstdint>
#include <limits>
#include <pj_base/builtin/mesh_map.hpp>
#include <pj_base/builtin/mesh_map_update.hpp>
#include <pj_base/expected.hpp>
#include <string>
#include <vector>

#include "navmap_msg_decoder.hpp"

namespace navmap_parser {

/// navmap_ros_interfaces/NavMapLayer type tags.
enum class LayerType : uint8_t { kU8 = 0, kF32 = 1, kF64 = 2, kI16 = 3 };

/// One per-NavCel layer, values widened to double.
struct NavMapLayer {
  std::string frame_id;  ///< Header frame; set only for a standalone NavMapLayer message.
  std::string name;
  LayerType type = LayerType::kF32;
  std::string unit;
  bool has_unknown = false;
  double unknown_value = std::numeric_limits<double>::quiet_NaN();
  bool has_geometry_id = false;
  uint64_t geometry_id = 0;
  /// Empty: `values` covers every NavCel. Otherwise values[k] belongs to NavCel indices[k].
  std::vector<uint32_t> indices;
  std::vector<double> values;
};

/// A group of NavCels expressed in one frame.
struct NavMapSurface {
  std::string frame_id;
  std::vector<uint32_t> navcels;
};

/// A connection between two non-adjacent NavCels (stairs, elevator, ...).
struct NavMapLink {
  uint32_t from_navcel = 0;
  uint32_t to_navcel = 0;
  uint8_t type = 0;
  bool bidirectional = false;
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
  /// Layers dropped because their data does not match the NavCel count.
  uint32_t dropped_layers = 0;
};

/// Validates a decoded NavMap and moves its arrays out of @p root. Fields
/// missing from older message revisions keep their defaults.
[[nodiscard]] PJ::Expected<NavMapData> extractNavMap(Node& root);

/// Reads a decoded NavMapLayer (standalone message or an element of NavMap.layers).
[[nodiscard]] NavMapLayer extractLayer(Node& node);

/// Builds the MeshMap:
/// - vertex records: x, y, z (float32), plus red, green, blue, alpha (uint8)
///   when the map has vertex colours;
/// - face records: vertex_indices, `surface` (uint32 surface index, or 0 when
///   the map has none), then one channel per layer. A layer with an unknown
///   value or listing only some NavCels becomes float32 with NaN for "no data";
///   an int16 `1/1000 log-odds` layer becomes the float32 occupancy probability.
///   Other layers keep their type.
/// - `frame_id` is the header frame: MeshMap has one frame per object, and
///   NavMap surfaces share the map frame in practice.
[[nodiscard]] PJ::sdk::MeshMap buildMeshMap(const NavMapData& map, int64_t timestamp_ns);

/// Builds the MeshMapUpdate for a standalone NavMapLayer message: face records
/// of `index` plus the layer channel, encoded as in buildMeshMap() (without
/// the sparse widening: an update only lists what it changes). Fails when the
/// data does not match the listed indices.
[[nodiscard]] PJ::Expected<PJ::sdk::MeshMapUpdate> buildMeshMapUpdate(const NavMapLayer& layer, int64_t timestamp_ns);

}  // namespace navmap_parser
