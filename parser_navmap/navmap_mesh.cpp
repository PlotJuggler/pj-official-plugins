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

using Datatype = PJ::sdk::PointField::Datatype;

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

Datatype datatypeOf(LayerType type) {
  switch (type) {
    case LayerType::kU8:
      return Datatype::kUint8;
    case LayerType::kF64:
      return Datatype::kFloat64;
    case LayerType::kI16:
      return Datatype::kInt16;
    case LayerType::kF32:
      break;
  }
  return Datatype::kFloat32;
}

bool isFloat(Datatype datatype) {
  return datatype == Datatype::kFloat32 || datatype == Datatype::kFloat64;
}

// One value stored as `datatype`. Integer values come from integer wire arrays,
// so they fit; a non-finite one (never produced here) is clamped to 0 rather
// than cast, which would be undefined.
void writeValue(uint8_t* dst, Datatype datatype, double value) {
  switch (datatype) {
    case Datatype::kUint8: {
      const auto v = std::isfinite(value) ? static_cast<uint8_t>(std::clamp(value, 0.0, 255.0)) : uint8_t{0};
      std::memcpy(dst, &v, sizeof(v));
      return;
    }
    case Datatype::kInt16: {
      const auto v = std::isfinite(value) ? static_cast<int16_t>(std::clamp(value, -32768.0, 32767.0)) : int16_t{0};
      std::memcpy(dst, &v, sizeof(v));
      return;
    }
    case Datatype::kUint32: {
      const auto v = std::isfinite(value) ? static_cast<uint32_t>(std::clamp(value, 0.0, 4294967295.0)) : 0U;
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

// The byte arrays a MeshMap / MeshMapUpdate views, kept alive by its anchor.
// Moving a vector keeps its buffer, so spans stay valid as blobs grows.
struct OwnedBlobs {
  std::vector<std::vector<uint8_t>> blobs;

  PJ::Span<const uint8_t> add(std::vector<uint8_t> bytes) {
    blobs.push_back(std::move(bytes));
    return {blobs.back().data(), blobs.back().size()};
  }
};

// `values` stored as `datatype`, tightly packed.
std::vector<uint8_t> encode(const std::vector<double>& values, Datatype datatype) {
  const size_t size = PJ::sdk::bytesPerElement(datatype);
  std::vector<uint8_t> bytes(values.size() * size);
  for (size_t i = 0; i < values.size(); ++i) {
    writeValue(bytes.data() + (i * size), datatype, values[i]);
  }
  return bytes;
}

template <typename T>
std::vector<uint8_t> rawBytes(const std::vector<T>& values) {
  std::vector<uint8_t> bytes(values.size() * sizeof(T));
  if (!bytes.empty()) {
    std::memcpy(bytes.data(), values.data(), bytes.size());
  }
  return bytes;
}

// The layer's unit, and its "unknown" value as the channel's no-data marker
// (NaN needs none: it is always "no data" in a float channel).
PJ::sdk::ChannelInfo infoFor(const NavMapLayer& layer) {
  PJ::sdk::ChannelInfo info;
  info.unit = layer.unit;
  if (layer.has_unknown && !std::isnan(layer.unknown_value)) {
    info.no_data = layer.unknown_value;
  }
  return info;
}

// The channel a layer is stored in: its name, unless that is empty or the
// `surface` channel this parser adds. Shared by maps and updates so both
// address the same channel.
std::string channelNameFor(const std::string& layer_name) {
  if (layer_name.empty()) {
    return "layer";
  }
  if (layer_name == "surface") {
    return "surface_layer";
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
  const auto declared = static_cast<uint8_t>(numberOf(node, "type").value_or(1.0));
  layer.type = declared <= 3 ? static_cast<LayerType>(declared) : LayerType::kF32;
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

  // The type tag names the data array; when that one is empty, fall back to
  // whichever array is filled, and take the type from it so values always fit.
  static constexpr std::pair<const char*, LayerType> kArrays[] = {
      {"data_u8", LayerType::kU8},
      {"data_f32", LayerType::kF32},
      {"data_f64", LayerType::kF64},
      {"data_i16", LayerType::kI16}};
  for (const auto& [name, type] : kArrays) {
    if (type == layer.type) {
      layer.values = arrayAsDoubles(node.find(name));
    }
  }
  if (layer.values.empty()) {
    for (const auto& [name, type] : kArrays) {
      layer.values = arrayAsDoubles(node.find(name));
      if (!layer.values.empty()) {
        layer.type = type;
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
      link.length = numberOf(node, "length").value_or(std::numeric_limits<double>::quiet_NaN());
      // A link to a missing NavCel cannot be drawn; skip it rather than lose the map.
      if (link.from_navcel < navcel_count && link.to_navcel < navcel_count) {
        map.links.push_back(link);
      }
    }
  }
  return map;
}

PJ::sdk::MeshMap buildMeshMap(const NavMapData& map, int64_t timestamp_ns) {
  using PJ::sdk::MeshChannel;
  auto owned = std::make_shared<OwnedBlobs>();
  PJ::sdk::MeshMap mesh;
  mesh.timestamp_ns = timestamp_ns;
  mesh.frame_id = map.frame_id;
  mesh.geometry_id = geometryIdText(map.has_geometry_id, map.geometry_id);
  mesh.vertex_count = static_cast<uint32_t>(map.x.size());
  mesh.face_count = static_cast<uint32_t>(map.v0.size());
  const size_t navcels = map.v0.size();

  // Geometry: NavMap keeps one array per axis and per corner; MeshMap wants them interleaved.
  std::vector<float> positions(map.x.size() * 3);
  for (size_t v = 0; v < map.x.size(); ++v) {
    positions[(3 * v) + 0] = map.x[v];
    positions[(3 * v) + 1] = map.y[v];
    positions[(3 * v) + 2] = map.z[v];
  }
  mesh.position_type = Datatype::kFloat32;
  mesh.positions = owned->add(rawBytes(positions));
  std::vector<uint32_t> indices(navcels * 3);
  for (size_t f = 0; f < navcels; ++f) {
    indices[(3 * f) + 0] = map.v0[f];
    indices[(3 * f) + 1] = map.v1[f];
    indices[(3 * f) + 2] = map.v2[f];
  }
  mesh.indices = owned->add(rawBytes(indices));

  // Vertex colour: the arrays are already one per channel.
  if (map.has_vertex_rgba) {
    const std::vector<uint8_t> opaque(map.x.size(), uint8_t{255});
    for (const auto& [name, values] :
         {std::pair<const char*, const std::vector<uint8_t>*>{"red", &map.r},
          {"green", &map.g},
          {"blue", &map.b},
          {"alpha", map.a.empty() ? &opaque : &map.a}}) {
      MeshChannel channel;
      channel.name = name;
      channel.target = MeshChannel::Target::kVertex;
      channel.datatype = Datatype::kUint8;
      channel.data = owned->add(*values);
      mesh.channels.push_back(std::move(channel));
    }
  }

  // The surface each NavCel belongs to, as a categorical channel labelled by frame.
  if (!map.surfaces.empty()) {
    std::vector<uint32_t> surface(navcels, 0U);
    PJ::sdk::ChannelInfo info;
    info.kind = PJ::sdk::ChannelKind::kCategorical;
    for (uint32_t s = 0; s < map.surfaces.size(); ++s) {
      for (const uint32_t f : map.surfaces[s].navcels) {
        surface[f] = s;
      }
      const std::string& frame = map.surfaces[s].frame_id;
      PJ::sdk::ChannelCategory category;
      category.value = s;
      category.label = frame.empty() ? "surface " + std::to_string(s) : frame;
      info.categories.push_back(std::move(category));
    }
    mesh.channels.push_back(
        {.name = "surface",
         .target = MeshChannel::Target::kFace,
         .datatype = Datatype::kUint32,
         .data = owned->add(rawBytes(surface)),
         .info = std::move(info)});
  }

  // One face channel per layer, in its own type.
  std::set<std::string> used_names{"surface"};
  for (const NavMapLayer& layer : map.layers) {
    Datatype datatype = datatypeOf(layer.type);
    PJ::sdk::ChannelInfo info = infoFor(layer);
    std::vector<double> values = layer.values;
    if (!layer.indices.empty()) {
      // Listed NavCels only: the rest have no data. An integer layer without an
      // "unknown" value has nothing to mark them with, so it widens to float.
      double fill = std::numeric_limits<double>::quiet_NaN();
      if (!isFloat(datatype)) {
        if (info.no_data.has_value()) {
          fill = *info.no_data;
        } else {
          datatype = Datatype::kFloat32;
        }
      }
      values.assign(navcels, fill);
      for (size_t k = 0; k < layer.indices.size(); ++k) {
        values[layer.indices[k]] = layer.values[k];
      }
    }
    mesh.channels.push_back(
        {.name = uniqueChannelName(channelNameFor(layer.name), used_names),
         .target = MeshChannel::Target::kFace,
         .datatype = datatype,
         .data = owned->add(encode(values, datatype)),
         .info = std::move(info)});
  }

  for (const NavMapLink& link : map.links) {
    mesh.links.push_back(
        {.from_face = link.from_navcel,
         .to_face = link.to_navcel,
         .kind = link.type,
         .bidirectional = link.bidirectional,
         .cost = link.length});
  }
  mesh.anchor = std::move(owned);
  return mesh;
}

PJ::Expected<PJ::sdk::MeshMapUpdate> buildMeshMapUpdate(const NavMapLayer& layer, int64_t timestamp_ns) {
  if (!layer.indices.empty() && layer.indices.size() != layer.values.size()) {
    return PJ::unexpected(std::string("NavMapLayer: indices and data have different lengths"));
  }
  auto owned = std::make_shared<OwnedBlobs>();
  PJ::sdk::MeshMapUpdate update;
  update.timestamp_ns = timestamp_ns;
  update.frame_id = layer.frame_id;
  update.geometry_id = geometryIdText(layer.has_geometry_id, layer.geometry_id);
  const Datatype datatype = datatypeOf(layer.type);
  PJ::sdk::MeshChannelPatch patch;
  patch.channel = {
      .name = channelNameFor(layer.name),
      .target = PJ::sdk::MeshChannel::Target::kFace,
      .datatype = datatype,
      .data = owned->add(encode(layer.values, datatype)),
      .info = infoFor(layer)};
  // No indices: a dense patch replacing the whole channel.
  patch.indices = layer.indices.empty() ? PJ::Span<const uint8_t>{} : owned->add(rawBytes(layer.indices));
  update.patches.push_back(std::move(patch));
  update.anchor = std::move(owned);
  return update;
}

}  // namespace navmap_parser
