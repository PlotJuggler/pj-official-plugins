// Copyright 2026 PlotJuggler contributors
// SPDX-License-Identifier: MIT

#include "pj_grid_map/grid_map_transcoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <pj_base/builtin/grid_map_codec.hpp>

namespace PJ {
namespace grid_map {

namespace {

using Datatype = sdk::PointField::Datatype;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

bool positiveFinite(double v) {
  return std::isfinite(v) && v > 0.0;
}

/// grid_map_core quantizes lengths to `size * resolution`; a length that
/// rounds to a different cell count contradicts the arrays.
bool lengthMatchesSize(double length, double resolution, uint32_t size) {
  return std::llround(length / resolution) == static_cast<long long>(size);
}

bool sameLayout(const MultiArrayDimension& a, const MultiArrayDimension& b) {
  return a.label == b.label && a.size == b.size && a.stride == b.stride;
}

}  // namespace

Expected<sdk::GridMap> transcodeGridMap(const GridMapMessage& msg) {
  // ---- Header-level checks, all before touching any array ----
  if (msg.layers.empty()) {
    return unexpected(std::string("GridMap: no layers"));
  }
  if (msg.layers.size() > kMaxLayers) {
    return unexpected(std::string("GridMap: too many layers (") + std::to_string(msg.layers.size()) + ")");
  }
  if (msg.data.size() != msg.layers.size()) {
    return unexpected(std::string("GridMap: layer count differs from data array count"));
  }
  {
    std::vector<std::string> names = msg.layers;
    std::sort(names.begin(), names.end());
    if (std::adjacent_find(names.begin(), names.end()) != names.end()) {
      return unexpected(std::string("GridMap: duplicate layer name"));
    }
  }
  std::vector<size_t> basic_indices;
  basic_indices.reserve(msg.basic_layers.size());
  for (const auto& name : msg.basic_layers) {
    const auto it = std::find(msg.layers.begin(), msg.layers.end(), name);
    if (it == msg.layers.end()) {
      return unexpected(std::string("GridMap: basic layer '") + name + "' is not in layers");
    }
    basic_indices.push_back(static_cast<size_t>(it - msg.layers.begin()));
  }
  if (!positiveFinite(msg.resolution) || !positiveFinite(msg.length_x) || !positiveFinite(msg.length_y)) {
    return unexpected(std::string("GridMap: resolution and lengths must be finite and positive"));
  }

  // ---- Layout from layer 0; every other layer must match it ----
  const auto& dims = msg.data.front().dims;
  if (dims.size() != 2 || dims[0].label != "column_index" || dims[1].label != "row_index") {
    return unexpected(std::string("GridMap: layer layout must be dim[0]=column_index, dim[1]=row_index"));
  }
  const uint32_t size_y = dims[0].size;
  const uint32_t size_x = dims[1].size;
  if (size_x == 0 || size_y == 0) {
    return unexpected(std::string("GridMap: zero-sized layer"));
  }
  const uint64_t cells = static_cast<uint64_t>(size_x) * size_y;
  if (cells > kMaxCells) {
    return unexpected(std::string("GridMap: too many cells (") + std::to_string(cells) + ")");
  }
  if (!lengthMatchesSize(msg.length_x, msg.resolution, size_x) ||
      !lengthMatchesSize(msg.length_y, msg.resolution, size_y)) {
    return unexpected(std::string("GridMap: length_x/length_y do not match the array sizes at this resolution"));
  }
  if (msg.outer_start_index >= size_x || msg.inner_start_index >= size_y) {
    return unexpected(std::string("GridMap: start index out of range"));
  }
  for (size_t k = 0; k < msg.data.size(); ++k) {
    const auto& layer = msg.data[k];
    if (layer.dims.size() != 2 || !sameLayout(layer.dims[0], dims[0]) || !sameLayout(layer.dims[1], dims[1])) {
      return unexpected(std::string("GridMap: layer '") + msg.layers[k] + "' has a different layout");
    }
    if (static_cast<uint64_t>(layer.data_offset) + cells > layer.data.size()) {
      return unexpected(std::string("GridMap: layer '") + msg.layers[k] + "' array is shorter than its layout");
    }
  }

  // ---- Transcode: column-major ring buffer -> row-major interleaved records ----
  const size_t layer_count = msg.layers.size();
  const uint32_t cell_stride = static_cast<uint32_t>(4 * layer_count);
  const uint32_t row_stride = cell_stride * size_x;
  auto owned = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(cells) * cell_stride);
  auto* out = reinterpret_cast<float*>(owned->data());

  for (uint32_t r = 0; r < size_y; ++r) {
    const uint32_t bj = (size_y - 1 - r + msg.inner_start_index) % size_y;
    for (uint32_t c = 0; c < size_x; ++c) {
      const uint32_t bi = (size_x - 1 - c + msg.outer_start_index) % size_x;
      const size_t src = static_cast<size_t>(bj) * size_x + bi;
      float* record = out + (static_cast<size_t>(r) * size_x + c) * layer_count;

      bool valid = true;
      for (const size_t b : basic_indices) {
        const auto& layer = msg.data[b];
        if (!std::isfinite(layer.data[layer.data_offset + src])) {
          valid = false;
          break;
        }
      }
      for (size_t k = 0; k < layer_count; ++k) {
        const auto& layer = msg.data[k];
        record[k] = valid ? layer.data[layer.data_offset + src] : kNaN;
      }
    }
  }

  sdk::GridMap grid;
  grid.origin.position = {
      .x = msg.center_x - size_x * msg.resolution / 2.0, .y = msg.center_y - size_y * msg.resolution / 2.0, .z = 0.0};
  grid.cell_size = {.x = msg.resolution, .y = msg.resolution};
  grid.column_count = size_x;
  grid.row_count = size_y;
  grid.cell_stride = cell_stride;
  grid.row_stride = row_stride;
  grid.fields.reserve(layer_count);
  for (size_t k = 0; k < layer_count; ++k) {
    grid.fields.push_back(
        {.name = msg.layers[k], .offset = static_cast<uint32_t>(4 * k), .datatype = Datatype::kFloat32, .count = 1});
  }
  grid.data = Span<const uint8_t>(owned->data(), owned->size());
  grid.anchor = std::move(owned);

  if (auto valid = validateGridMap(grid); !valid) {
    return unexpected(std::string("GridMap: ") + std::move(valid).error());
  }
  return grid;
}

sdk::PointField::Datatype mapFoxglovePackedElementType(uint64_t type) {
  switch (type) {
    case 1:
      return Datatype::kUint8;
    case 2:
      return Datatype::kInt8;
    case 3:
      return Datatype::kUint16;
    case 4:
      return Datatype::kInt16;
    case 5:
      return Datatype::kUint32;
    case 6:
      return Datatype::kInt32;
    case 7:
      return Datatype::kFloat32;
    case 8:
      return Datatype::kFloat64;
    default:
      return Datatype::kUnknown;
  }
}

Expected<void> finalizeFoxgloveGrid(sdk::GridMap& grid) {
  for (const auto& field : grid.fields) {
    if (field.datatype == Datatype::kUnknown) {
      return unexpected(std::string("Grid: field '") + field.name + "' has an unknown datatype");
    }
  }
  if (grid.data.empty()) {
    grid.row_count = 0;
  } else {
    if (grid.row_stride == 0) {
      return unexpected(std::string("Grid: row_stride is 0 but data is not empty"));
    }
    if (grid.data.size() % grid.row_stride != 0) {
      return unexpected(std::string("Grid: data length is not a whole number of rows"));
    }
    if (static_cast<uint64_t>(grid.column_count) * grid.cell_stride > grid.row_stride) {
      return unexpected(std::string("Grid: column_count * cell_stride exceeds row_stride"));
    }
    grid.row_count = static_cast<uint32_t>(grid.data.size() / grid.row_stride);
  }
  if (auto valid = validateGridMap(grid); !valid) {
    return unexpected(std::string("Grid: ") + std::move(valid).error());
  }
  return okStatus();
}

}  // namespace grid_map
}  // namespace PJ
