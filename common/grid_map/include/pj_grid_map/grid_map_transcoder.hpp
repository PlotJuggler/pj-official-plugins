// pj_grid_map — grid-map message -> sdk::GridMap conversion shared by the
// message parsers (parser_ros: grid_map_msgs/GridMap + foxglove_msgs/Grid,
// parser_protobuf: foxglove.Grid). Qt-free; depends only on pj_base types.
//
// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <pj_base/builtin/grid_map.hpp>
#include <pj_base/expected.hpp>
#include <pj_base/span.hpp>
#include <string>
#include <vector>

namespace PJ {
namespace grid_map {

/// Resource caps checked before any allocation, so a corrupt header cannot
/// request gigabytes.
constexpr uint64_t kMaxCells = 16'777'216;  ///< size_x * size_y (4096 x 4096)
constexpr size_t kMaxLayers = 64;

/// One std_msgs/MultiArrayDimension.
struct MultiArrayDimension {
  std::string label;
  uint32_t size = 0;
  uint32_t stride = 0;
};

/// One std_msgs/Float32MultiArray (a grid_map layer). `data` is borrowed for
/// the duration of transcodeGridMap() only.
struct LayerArray {
  std::vector<MultiArrayDimension> dims;
  uint32_t data_offset = 0;  ///< Leading padding, in float elements (honored).
  Span<const float> data;
};

/// The decoded parts of a grid_map_msgs/GridMap the transcoder needs. The
/// envelope (header stamp / frame_id) is stamped by the caller on the result.
/// Only the center's x/y are taken: grid_map_ros ignores the pose's z and
/// orientation on both directions, so a map is always axis-aligned in frame_id.
struct GridMapMessage {
  double resolution = 0.0;
  double length_x = 0.0;
  double length_y = 0.0;
  double center_x = 0.0;
  double center_y = 0.0;
  std::vector<std::string> layers;
  std::vector<std::string> basic_layers;
  std::vector<LayerArray> data;  ///< One per layer, same order as `layers`.
  uint16_t outer_start_index = 0;
  uint16_t inner_start_index = 0;
};

/// Transcodes a grid_map message into an owned canonical GridMap: one float32
/// field per layer (message order), `cell_size = (resolution, resolution)`,
/// `column_count = size_x`, `row_count = size_y`, identity-orientation origin
/// at the map's -x/-y corner (z = 0: elevation values are absolute heights).
///
/// grid_map stores each layer as an Eigen matrix (rows = size_x, cols = size_y)
/// serialized column-major, with index (0,0) at the +x/+y corner and a ring
/// buffer whose logical origin is `(outer_start_index, inner_start_index)`.
/// Canonical cell (c, r) therefore reads
///   i = size_x-1-c, j = size_y-1-r, value = data[offset + ((j+inner)%size_y)*size_x + (i+outer)%size_x].
///
/// basic_layers: a cell is valid only when every basic layer is finite there;
/// otherwise NaN is written into ALL of that cell's fields (grid_map semantics,
/// no extra validity mask needed).
///
/// Rejects (Expected error) any layout grid_map_core would not have produced
/// (see the checks in transcodeGridMap), capped at kMaxCells / kMaxLayers; the
/// result is passed through validateGridMap() before it is returned.
[[nodiscard]] Expected<sdk::GridMap> transcodeGridMap(const GridMapMessage& msg);

/// Foxglove PackedElementField numeric type -> canonical PointField datatype.
/// Foxglove numbers UINT8=1, INT8=2, UINT16=3, INT16=4, UINT32=5, INT32=6,
/// FLOAT32=7, FLOAT64=8 — signed/unsigned swapped relative to ROS/SDK. Anything
/// else (including UNKNOWN=0) maps to kUnknown, which finalizeFoxgloveGrid rejects.
[[nodiscard]] sdk::PointField::Datatype mapFoxglovePackedElementType(uint64_t type);

/// Completes a GridMap built from a foxglove Grid (ROS or protobuf), whose wire
/// carries no row count: derives `row_count = data.size() / row_stride`,
/// rejecting a non-empty `data` with `row_stride == 0` or a length that is not
/// a whole number of rows, then runs validateGridMap(). An empty `data` yields
/// `row_count = 0`.
[[nodiscard]] Expected<void> finalizeFoxgloveGrid(sdk::GridMap& grid);

}  // namespace grid_map
}  // namespace PJ
