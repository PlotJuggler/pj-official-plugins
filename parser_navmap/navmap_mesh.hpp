/**
 * @file navmap_mesh.hpp
 * @brief NavMap / NavMapLayer messages -> sdk::MeshMap / sdk::MeshMapUpdate.
 *
 * Layers pass through as MeshMap channels in their own type; only the geometry
 * is repacked (NavMap keeps one array per axis and per corner, MeshMap wants
 * them interleaved). Styling is left to the 3D view.
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
  LayerType type = LayerType::kF32;  ///< the type of the data array actually present
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
  /// Layers dropped because their data does not match the NavCel count.
  uint32_t dropped_layers = 0;
};

/// Validates a decoded NavMap and moves its arrays out of @p root. Fields
/// missing from older message revisions keep their defaults.
[[nodiscard]] PJ::Expected<NavMapData> extractNavMap(Node& root);

/// Reads a decoded NavMapLayer (standalone message or an element of NavMap.layers).
[[nodiscard]] NavMapLayer extractLayer(Node& node);

/// Builds the MeshMap: positions and indices interleaved from NavMap's per-axis
/// and per-corner arrays; `red`/`green`/`blue`/`alpha` vertex channels when the
/// map has vertex colours; a categorical uint32 `surface` face channel (labels:
/// the surface frames) when it has surfaces; one face channel per layer, in the
/// layer's own type, carrying its unit and its "unknown" value as
/// `info.no_data`. A layer listing only some NavCels marks the others with that
/// value (NaN for a float layer; an integer layer without one widens to
/// float32). A layer name that is empty or `surface` is renamed (channelNameFor),
/// a repeated one gets a numeric suffix. Links keep their type and length.
/// `frame_id` is the header frame: MeshMap has one frame per object, and NavMap
/// surfaces share the map frame in practice.
[[nodiscard]] PJ::sdk::MeshMap buildMeshMap(const NavMapData& map, int64_t timestamp_ns);

/// Builds the MeshMapUpdate for a standalone NavMapLayer message: one face
/// patch for the layer's channel, dense when the layer lists no indices, sparse
/// otherwise, in the layer's own type. Fails when the data does not match the
/// listed indices.
[[nodiscard]] PJ::Expected<PJ::sdk::MeshMapUpdate> buildMeshMapUpdate(const NavMapLayer& layer, int64_t timestamp_ns);

}  // namespace navmap_parser
