#include "navmap_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace navmap_parser {

namespace {

using PJ::sdk::PointField;
using Datatype = PJ::sdk::PointField::Datatype;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Moves a numeric array out of a decoded field, converting element-wise when
// the wire type differs from T. A missing or non-array field yields empty.
template <typename T>
std::vector<T> takeArray(Node& node, std::string_view name) {
  Value* value = node.find(name);
  if (value == nullptr || value->kind != Value::Kind::kNumberArray) {
    return {};
  }
  if (auto* exact = std::get_if<std::vector<T>>(&value->numbers)) {
    return std::move(*exact);
  }
  return std::visit(
      [](const auto& elements) {
        std::vector<T> out;
        out.reserve(elements.size());
        for (const auto element : elements) {
          out.push_back(static_cast<T>(element));
        }
        return out;
      },
      value->numbers);
}

std::string takeString(Node& node, std::string_view name) {
  Value* value = node.find(name);
  return value != nullptr && value->kind == Value::Kind::kString ? std::move(value->text) : std::string();
}

std::optional<double> numberOf(const Node& node, std::string_view name) {
  const Value* value = node.find(name);
  if (value == nullptr || value->kind != Value::Kind::kNumber) {
    return std::nullopt;
  }
  return value->number;
}

Node* nodeOf(Node& node, std::string_view name) {
  Value* value = node.find(name);
  return value != nullptr && value->kind == Value::Kind::kNode && !value->nodes.empty() ? &value->nodes.front()
                                                                                        : nullptr;
}

std::vector<Node>* nodeArrayOf(Node& node, std::string_view name) {
  Value* value = node.find(name);
  return value != nullptr && value->kind == Value::Kind::kNodeArray ? &value->nodes : nullptr;
}

bool allBelow(const std::vector<uint32_t>& indices, size_t limit) {
  return std::all_of(indices.begin(), indices.end(), [limit](uint32_t index) { return index < limit; });
}

// --- Channel encoding --------------------------------------------------------

// How one layer is stored as a MeshMap channel.
struct ChannelEncoding {
  Datatype datatype = Datatype::kFloat32;
  bool log_odds_to_probability = false;
  bool unknown_to_nan = false;
};

bool isLogOdds(const NavMapLayer& layer) {
  return layer.type == LayerType::kI16 && layer.unit == "1/1000 log-odds";
}

ChannelEncoding encodingFor(const NavMapLayer& layer, bool sparse_means_no_data) {
  if (isLogOdds(layer)) {
    return {.datatype = Datatype::kFloat32, .log_odds_to_probability = true, .unknown_to_nan = layer.has_unknown};
  }
  if (layer.has_unknown || (sparse_means_no_data && !layer.indices.empty())) {
    // MeshMap has no integer "no data" sentinel: widen so NaN can carry it.
    return {
        .datatype = layer.type == LayerType::kF64 ? Datatype::kFloat64 : Datatype::kFloat32,
        .unknown_to_nan = layer.has_unknown};
  }
  switch (layer.type) {
    case LayerType::kU8:
      return {.datatype = Datatype::kUint8};
    case LayerType::kF64:
      return {.datatype = Datatype::kFloat64};
    case LayerType::kI16:
      return {.datatype = Datatype::kInt16};
    case LayerType::kF32:
      break;
  }
  return {.datatype = Datatype::kFloat32};
}

double encodeValue(const NavMapLayer& layer, const ChannelEncoding& encoding, double value) {
  if (encoding.unknown_to_nan &&
      ((std::isnan(layer.unknown_value) && std::isnan(value)) || value == layer.unknown_value)) {
    return kNaN;
  }
  if (encoding.log_odds_to_probability) {
    return 1.0 / (1.0 + std::exp(-value / 1000.0));
  }
  return value;
}

void writeValue(uint8_t* dst, Datatype datatype, double value) {
  switch (datatype) {
    case Datatype::kUint8: {
      const auto v = static_cast<uint8_t>(value);
      std::memcpy(dst, &v, sizeof(v));
      return;
    }
    case Datatype::kInt16: {
      const auto v = static_cast<int16_t>(value);
      std::memcpy(dst, &v, sizeof(v));
      return;
    }
    case Datatype::kFloat64:
      std::memcpy(dst, &value, sizeof(value));
      return;
    default: {
      const auto v = static_cast<float>(value);
      std::memcpy(dst, &v, sizeof(v));
      return;
    }
  }
}

// Appends a field at the next offset aligned to its element size; returns the offset.
uint32_t addField(
    std::vector<PointField>& fields, uint32_t& stride, std::string name, Datatype datatype, uint32_t count = 1) {
  const uint32_t size = PJ::sdk::bytesPerElement(datatype);
  const uint32_t offset = (stride + size - 1) / size * size;
  fields.push_back({.name = std::move(name), .offset = offset, .datatype = datatype, .count = count});
  stride = offset + (size * count);
  return offset;
}

uint32_t roundUp(uint32_t value, uint32_t multiple) {
  return (value + multiple - 1) / multiple * multiple;
}

// The channel a layer is stored in: its name, unless that is empty or one of
// the record's own fields. Shared by maps and updates so both address the same
// channel.
std::string channelNameFor(const std::string& layer_name) {
  if (layer_name.empty()) {
    return "layer";
  }
  if (layer_name == PJ::sdk::kMeshMapVertexIndicesField || layer_name == "surface" ||
      layer_name == PJ::sdk::kMeshMapUpdateIndexField) {
    return layer_name + "_layer";
  }
  return layer_name;
}

// A later layer with an already-used name gets a numeric suffix.
std::string uniqueChannelName(const std::string& wanted, std::set<std::string>& used) {
  std::string name = wanted;
  for (int suffix = 2; used.contains(name); ++suffix) {
    name = wanted + "_" + std::to_string(suffix);
  }
  used.insert(name);
  return name;
}

std::string geometryIdText(bool has_id, uint64_t id) {
  return has_id && id != 0 ? std::to_string(id) : std::string();
}

}  // namespace

NavMapLayer extractLayer(Node& node) {
  NavMapLayer layer;
  if (Node* header = nodeOf(node, "header")) {
    layer.frame_id = takeString(*header, "frame_id");
  }
  layer.name = takeString(node, "name");
  layer.type = static_cast<LayerType>(static_cast<uint8_t>(numberOf(node, "type").value_or(1.0)));
  layer.unit = takeString(node, "unit");
  layer.has_unknown = numberOf(node, "has_unknown").value_or(0.0) != 0.0;
  if (const auto unknown = numberOf(node, "unknown_value")) {
    layer.unknown_value = *unknown;
  }
  if (const Value* geometry_id = node.find("geometry_id")) {
    layer.has_geometry_id = true;
    layer.geometry_id = geometry_id->unsigned_bits;
  }
  layer.indices = takeArray<uint32_t>(node, "indices");

  // The type tag names the data array; fall back to whichever array is filled.
  const char* by_type = "data_f32";
  switch (layer.type) {
    case LayerType::kU8:
      by_type = "data_u8";
      break;
    case LayerType::kF64:
      by_type = "data_f64";
      break;
    case LayerType::kI16:
      by_type = "data_i16";
      break;
    case LayerType::kF32:
      break;
  }
  layer.values = arrayAsDoubles(node.find(by_type));
  if (layer.values.empty()) {
    for (const char* name : {"data_u8", "data_f32", "data_f64", "data_i16"}) {
      layer.values = arrayAsDoubles(node.find(name));
      if (!layer.values.empty()) {
        break;
      }
    }
  }
  return layer;
}

PJ::Expected<NavMapData> extractNavMap(Node& root) {
  NavMapData map;
  if (Node* header = nodeOf(root, "header")) {
    map.frame_id = takeString(*header, "frame_id");
  }

  map.x = takeArray<float>(root, "positions_x");
  map.y = takeArray<float>(root, "positions_y");
  map.z = takeArray<float>(root, "positions_z");
  const size_t vertex_count = map.x.size();
  if (map.y.size() != vertex_count || map.z.size() != vertex_count) {
    return PJ::unexpected(std::string("NavMap: positions_x/y/z have different lengths"));
  }

  map.r = takeArray<uint8_t>(root, "colors_r");
  map.g = takeArray<uint8_t>(root, "colors_g");
  map.b = takeArray<uint8_t>(root, "colors_b");
  map.a = takeArray<uint8_t>(root, "colors_a");
  map.has_vertex_rgba = numberOf(root, "has_vertex_rgba").value_or(0.0) != 0.0 && vertex_count != 0 &&
                        map.r.size() == vertex_count && map.g.size() == vertex_count && map.b.size() == vertex_count;
  if (map.a.size() != vertex_count) {
    map.a.clear();  // opaque
  }

  map.v0 = takeArray<uint32_t>(root, "navcels_v0");
  map.v1 = takeArray<uint32_t>(root, "navcels_v1");
  map.v2 = takeArray<uint32_t>(root, "navcels_v2");
  const size_t navcel_count = map.v0.size();
  if (map.v1.size() != navcel_count || map.v2.size() != navcel_count) {
    return PJ::unexpected(std::string("NavMap: navcels_v0/v1/v2 have different lengths"));
  }
  if (!allBelow(map.v0, vertex_count) || !allBelow(map.v1, vertex_count) || !allBelow(map.v2, vertex_count)) {
    return PJ::unexpected(std::string("NavMap: a NavCel references a vertex out of range"));
  }

  if (std::vector<Node>* surfaces = nodeArrayOf(root, "surfaces")) {
    for (Node& node : *surfaces) {
      NavMapSurface surface;
      surface.frame_id = takeString(node, "frame_id");
      surface.navcels = takeArray<uint32_t>(node, "navcels");
      if (!allBelow(surface.navcels, navcel_count)) {
        return PJ::unexpected(std::string("NavMap: a surface references a NavCel out of range"));
      }
      map.surfaces.push_back(std::move(surface));
    }
  }

  if (std::vector<Node>* layers = nodeArrayOf(root, "layers")) {
    for (Node& node : *layers) {
      NavMapLayer layer = extractLayer(node);
      const bool valid = layer.indices.empty()
                             ? layer.values.size() == navcel_count
                             : layer.values.size() == layer.indices.size() && allBelow(layer.indices, navcel_count);
      if (valid) {
        map.layers.push_back(std::move(layer));
      } else {
        ++map.dropped_layers;
      }
    }
  }

  if (const Value* geometry_id = root.find("geometry_id"); geometry_id != nullptr) {
    map.has_geometry_id = true;
    map.geometry_id = geometry_id->unsigned_bits;
  }

  if (std::vector<Node>* links = nodeArrayOf(root, "links")) {
    for (const Node& node : *links) {
      NavMapLink link;
      link.from_navcel = static_cast<uint32_t>(numberOf(node, "from_navcel").value_or(0.0));
      link.to_navcel = static_cast<uint32_t>(numberOf(node, "to_navcel").value_or(0.0));
      link.type = static_cast<uint8_t>(numberOf(node, "type").value_or(0.0));
      link.bidirectional = numberOf(node, "bidirectional").value_or(0.0) != 0.0;
      // A link to a missing NavCel cannot be drawn; skip it rather than lose the map.
      if (link.from_navcel < navcel_count && link.to_navcel < navcel_count) {
        map.links.push_back(link);
      }
    }
  }
  return map;
}

PJ::sdk::MeshMap buildMeshMap(const NavMapData& map, int64_t timestamp_ns) {
  PJ::sdk::MeshMap mesh;
  mesh.timestamp_ns = timestamp_ns;
  mesh.frame_id = map.frame_id;
  mesh.geometry_id = geometryIdText(map.has_geometry_id, map.geometry_id);
  mesh.vertex_count = static_cast<uint32_t>(map.x.size());
  mesh.face_count = static_cast<uint32_t>(map.v0.size());

  // Vertex record layout.
  uint32_t vertex_stride = 0;
  addField(mesh.vertex_fields, vertex_stride, "x", Datatype::kFloat32);
  addField(mesh.vertex_fields, vertex_stride, "y", Datatype::kFloat32);
  addField(mesh.vertex_fields, vertex_stride, "z", Datatype::kFloat32);
  uint32_t color_offset = 0;
  if (map.has_vertex_rgba) {
    color_offset = addField(mesh.vertex_fields, vertex_stride, "red", Datatype::kUint8);
    addField(mesh.vertex_fields, vertex_stride, "green", Datatype::kUint8);
    addField(mesh.vertex_fields, vertex_stride, "blue", Datatype::kUint8);
    addField(mesh.vertex_fields, vertex_stride, "alpha", Datatype::kUint8);
  }
  mesh.vertex_stride = roundUp(vertex_stride, 4);

  // Face record layout: indices, surface, then one channel per layer.
  uint32_t face_stride = 0;
  addField(mesh.face_fields, face_stride, std::string(PJ::sdk::kMeshMapVertexIndicesField), Datatype::kUint32, 3);
  const uint32_t surface_offset = addField(mesh.face_fields, face_stride, "surface", Datatype::kUint32);
  std::set<std::string> used_names{std::string(PJ::sdk::kMeshMapVertexIndicesField), "surface"};
  std::vector<ChannelEncoding> encodings;
  std::vector<uint32_t> layer_offsets;
  uint32_t max_align = 4;
  for (const NavMapLayer& layer : map.layers) {
    encodings.push_back(encodingFor(layer, /*sparse_means_no_data=*/true));
    max_align = std::max(max_align, PJ::sdk::bytesPerElement(encodings.back().datatype));
    layer_offsets.push_back(addField(
        mesh.face_fields, face_stride, uniqueChannelName(channelNameFor(layer.name), used_names),
        encodings.back().datatype));
  }
  mesh.face_stride = roundUp(face_stride, max_align);
  mesh.face_data_offset = roundUp(mesh.vertex_count * mesh.vertex_stride, 8);

  auto buffer = std::make_shared<std::vector<uint8_t>>(
      mesh.face_data_offset + (static_cast<size_t>(mesh.face_count) * mesh.face_stride), uint8_t{0});

  for (uint32_t v = 0; v < mesh.vertex_count; ++v) {
    uint8_t* record = buffer->data() + (static_cast<size_t>(v) * mesh.vertex_stride);
    std::memcpy(record, &map.x[v], sizeof(float));
    std::memcpy(record + 4, &map.y[v], sizeof(float));
    std::memcpy(record + 8, &map.z[v], sizeof(float));
    if (map.has_vertex_rgba) {
      record[color_offset] = map.r[v];
      record[color_offset + 1] = map.g[v];
      record[color_offset + 2] = map.b[v];
      record[color_offset + 3] = map.a.empty() ? uint8_t{255} : map.a[v];
    }
  }

  uint8_t* faces = buffer->data() + mesh.face_data_offset;
  for (uint32_t f = 0; f < mesh.face_count; ++f) {
    uint8_t* record = faces + (static_cast<size_t>(f) * mesh.face_stride);
    const uint32_t indices[3] = {map.v0[f], map.v1[f], map.v2[f]};
    std::memcpy(record, indices, sizeof(indices));
  }
  for (uint32_t s = 0; s < map.surfaces.size(); ++s) {
    for (const uint32_t f : map.surfaces[s].navcels) {
      std::memcpy(faces + (static_cast<size_t>(f) * mesh.face_stride) + surface_offset, &s, sizeof(s));
    }
  }
  for (size_t l = 0; l < map.layers.size(); ++l) {
    const NavMapLayer& layer = map.layers[l];
    const ChannelEncoding& encoding = encodings[l];
    auto write = [&](uint32_t face, double value) {
      writeValue(
          faces + (static_cast<size_t>(face) * mesh.face_stride) + layer_offsets[l], encoding.datatype,
          encodeValue(layer, encoding, value));
    };
    if (layer.indices.empty()) {
      for (uint32_t f = 0; f < mesh.face_count; ++f) {
        write(f, layer.values[f]);
      }
    } else {
      // Unlisted NavCels have no data (the channel was widened to float).
      for (uint32_t f = 0; f < mesh.face_count; ++f) {
        writeValue(faces + (static_cast<size_t>(f) * mesh.face_stride) + layer_offsets[l], encoding.datatype, kNaN);
      }
      for (size_t k = 0; k < layer.indices.size(); ++k) {
        write(layer.indices[k], layer.values[k]);
      }
    }
  }

  for (const NavMapLink& link : map.links) {
    mesh.links.push_back(
        {.from_face = link.from_navcel,
         .to_face = link.to_navcel,
         .kind = link.type,
         .bidirectional = link.bidirectional});
  }

  mesh.data = PJ::Span<const uint8_t>(buffer->data(), buffer->size());
  mesh.anchor = std::move(buffer);
  return mesh;
}

PJ::Expected<PJ::sdk::MeshMapUpdate> buildMeshMapUpdate(const NavMapLayer& layer, int64_t timestamp_ns) {
  if (!layer.indices.empty() && layer.indices.size() != layer.values.size()) {
    return PJ::unexpected(std::string("NavMapLayer: indices and data have different lengths"));
  }
  PJ::sdk::MeshMapUpdate update;
  update.timestamp_ns = timestamp_ns;
  update.frame_id = layer.frame_id;
  update.geometry_id = geometryIdText(layer.has_geometry_id, layer.geometry_id);
  update.target = PJ::sdk::MeshMapUpdate::Target::kFaces;
  update.count = static_cast<uint32_t>(layer.values.size());

  const ChannelEncoding encoding = encodingFor(layer, /*sparse_means_no_data=*/false);
  uint32_t stride = 0;
  addField(update.fields, stride, std::string(PJ::sdk::kMeshMapUpdateIndexField), Datatype::kUint32);
  const uint32_t value_offset = addField(update.fields, stride, channelNameFor(layer.name), encoding.datatype);
  update.stride = roundUp(stride, std::max<uint32_t>(4, PJ::sdk::bytesPerElement(encoding.datatype)));

  auto buffer = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(update.count) * update.stride, uint8_t{0});
  for (uint32_t k = 0; k < update.count; ++k) {
    uint8_t* record = buffer->data() + (static_cast<size_t>(k) * update.stride);
    const uint32_t index = layer.indices.empty() ? k : layer.indices[k];
    std::memcpy(record, &index, sizeof(index));
    writeValue(record + value_offset, encoding.datatype, encodeValue(layer, encoding, layer.values[k]));
  }
  update.data = PJ::Span<const uint8_t>(buffer->data(), buffer->size());
  update.anchor = std::move(buffer);
  return update;
}

}  // namespace navmap_parser
