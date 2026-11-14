#include "navmap_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace navmap_parser {

namespace {

using Datatype = PJ::sdk::PointField::Datatype;

// `value` as T when it is finite and in T's range, else nullopt (casting it
// would be undefined). Only a hostile schema, declaring an integer field as a
// floating or wider type, produces a failing value.
template <typename T, typename S>
std::optional<T> checkedCast(S value) {
  static_assert(std::is_integral_v<T> && sizeof(T) <= sizeof(uint32_t));
  if constexpr (std::is_floating_point_v<S>) {
    // Both bounds are exact doubles for T up to 32 bits; NaN fails both.
    if (!(value > static_cast<double>(std::numeric_limits<T>::min()) - 1.0 &&
          value < static_cast<double>(std::numeric_limits<T>::max()) + 1.0)) {
      return std::nullopt;
    }
    return static_cast<T>(value);
  } else {
    return std::in_range<T>(value) ? std::optional<T>(static_cast<T>(value)) : std::nullopt;
  }
}

// Moves a numeric array out of a decoded field, converting element-wise when
// the wire type differs from T. A missing or non-array field yields empty. An
// integer element that does not fit T becomes T's maximum, which every index
// bound check rejects, so its layer, surface or map fails validation.
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
          if constexpr (std::is_integral_v<T>) {
            out.push_back(checkedCast<T>(element).value_or(std::numeric_limits<T>::max()));
          } else {
            out.push_back(static_cast<T>(element));
          }
        }
        return out;
      },
      value->numbers);
}

std::string takeString(Node& node, std::string_view name) {
  Value* value = node.find(name);
  return value != nullptr && value->kind == Value::Kind::kString ? std::move(value->text) : std::string();
}

// An integer field as T; nullopt when it is missing or does not fit T.
template <typename T>
std::optional<T> integerOf(const Node& node, std::string_view name) {
  const auto number = numberOf(node, name);
  return number ? checkedCast<T>(*number) : std::nullopt;
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

size_t sizeOf(const NumberArray& values) {
  return std::visit([](const auto& elements) { return elements.size(); }, values);
}

size_t elementSizeOf(const NumberArray& values) {
  return std::visit(
      [](const auto& elements) { return sizeof(typename std::decay_t<decltype(elements)>::value_type); }, values);
}

// --- Channel encoding --------------------------------------------------------

// The channel datatype that stores T as is; kUnknown for the 64-bit integers,
// which MeshMap channels cannot hold.
template <typename T>
constexpr Datatype datatypeFor() {
  if constexpr (std::is_same_v<T, uint8_t>) {
    return Datatype::kUint8;
  } else if constexpr (std::is_same_v<T, int8_t>) {
    return Datatype::kInt8;
  } else if constexpr (std::is_same_v<T, int16_t>) {
    return Datatype::kInt16;
  } else if constexpr (std::is_same_v<T, uint16_t>) {
    return Datatype::kUint16;
  } else if constexpr (std::is_same_v<T, int32_t>) {
    return Datatype::kInt32;
  } else if constexpr (std::is_same_v<T, uint32_t>) {
    return Datatype::kUint32;
  } else if constexpr (std::is_same_v<T, float>) {
    return Datatype::kFloat32;
  } else if constexpr (std::is_same_v<T, double>) {
    return Datatype::kFloat64;
  } else {
    return Datatype::kUnknown;
  }
}

// The arrays a MeshMap / MeshMapUpdate views, kept alive by its anchor, in
// their own type. Moving a vector keeps its buffer, so spans stay valid as
// `arrays` grows.
struct OwnedBlobs {
  std::vector<NumberArray> arrays;

  template <typename T>
  PJ::Span<const uint8_t> add(std::vector<T> values) {
    const auto& stored = std::get<std::vector<T>>(arrays.emplace_back(std::move(values)));
    return {reinterpret_cast<const uint8_t*>(stored.data()), stored.size() * sizeof(T)};
  }
};

// Three per-axis (or per-corner) arrays as one xyz-interleaved array.
template <typename T>
std::vector<T> interleave(const std::vector<T>& a, const std::vector<T>& b, const std::vector<T>& c) {
  std::vector<T> out;
  out.reserve(a.size() * 3);
  for (size_t i = 0; i < a.size(); ++i) {
    out.push_back(a[i]);
    out.push_back(b[i]);
    out.push_back(c[i]);
  }
  return out;
}

// A layer is stored in the channel named after it. A name that is empty or
// collides with a channel this parser adds (`surface`) or a MeshMap channel
// convention cannot be one, and renaming it would need state shared by a map
// and its updates, so such a layer is dropped. Shared by maps and updates so
// both address the same channel.
bool isLayerChannelName(std::string_view name) {
  static constexpr std::string_view kReserved[] = {"surface", "red", "green", "blue", "alpha", "normal", "uv"};
  return !name.empty() && std::find(std::begin(kReserved), std::end(kReserved), name) == std::end(kReserved);
}

// Whether `value` is exactly a value of T (always for a floating T).
template <typename T>
bool representable(double value) {
  if constexpr (std::is_floating_point_v<T>) {
    return true;
  } else {
    return value == std::trunc(value) && value >= static_cast<double>(std::numeric_limits<T>::min()) &&
           value <= static_cast<double>(std::numeric_limits<T>::max());
  }
}

// A layer's channel datatype is the type of the array it carries, in a map and
// in an update alike, so the two agree by construction. kUnknown for the 64-bit
// integers, which no MeshMap channel holds.
Datatype datatypeOf(const NavMapLayer& layer) {
  return std::visit(
      [](const auto& elements) { return datatypeFor<typename std::decay_t<decltype(elements)>::value_type>(); },
      layer.values);
}

// The layer's unit, and its "unknown" value as the channel's no-data marker
// when the channel's type can hold it (an integer channel cannot hold -1.5 or
// 300 in uint8, so it then declares none). NaN needs none: it is always "no
// data" in a float channel.
PJ::sdk::ChannelInfo infoFor(const NavMapLayer& layer) {
  PJ::sdk::ChannelInfo info;
  info.unit = layer.unit;
  const bool holds = std::visit(
      [&](const auto& elements) {
        return representable<typename std::decay_t<decltype(elements)>::value_type>(layer.unknown_value);
      },
      layer.values);
  if (layer.has_unknown && !std::isnan(layer.unknown_value) && holds) {
    info.no_data = layer.unknown_value;
  }
  return info;
}

// Whether a sparse layer inside a map can be stored: the NavCels it does not
// list need a fill value, the no-data value or, in a float channel, NaN.
bool fillable(const NavMapLayer& layer, const PJ::sdk::ChannelInfo& info) {
  return layer.indices.empty() || info.no_data.has_value() || datatypeOf(layer) == Datatype::kFloat32 ||
         datatypeOf(layer) == Datatype::kFloat64;
}

// `values` scattered over `size` elements, the rest `fill`, when `scatter`
// lists their elements; else moved as they are.
template <typename T>
std::vector<T> layerValues(std::vector<T>& values, const std::vector<uint32_t>& scatter, size_t size, T fill) {
  if (scatter.empty()) {
    return std::move(values);
  }
  std::vector<T> out(size, fill);
  for (size_t k = 0; k < scatter.size(); ++k) {
    out[scatter[k]] = values[k];
  }
  return out;
}

// Adds a layer's channel data in its own type. Scattered elements not listed
// hold the no-data value, else NaN (extractNavMap() drops an integer layer
// that would need a fill without a no-data value).
PJ::Span<const uint8_t> addLayerData(
    OwnedBlobs& owned, NumberArray& values, const std::vector<uint32_t>& scatter, size_t size,
    const PJ::sdk::ChannelInfo& info) {
  return std::visit(
      [&](auto& elements) {
        using T = typename std::decay_t<decltype(elements)>::value_type;
        T fill{};
        if (info.no_data.has_value()) {
          fill = static_cast<T>(*info.no_data);
        } else if constexpr (std::is_floating_point_v<T>) {
          fill = std::numeric_limits<T>::quiet_NaN();
        }
        return owned.add(layerValues<T>(elements, scatter, size, fill));
      },
      values);
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

  // The type tag (0 U8, 1 F32, 2 F64, 3 I16) names the data array; an unknown
  // tag means F32. When that array is empty, fall back to whichever is filled.
  static constexpr const char* kDataArrays[] = {"data_u8", "data_f32", "data_f64", "data_i16"};
  auto filled = [&](const char* name) -> NumberArray* {
    Value* value = node.find(name);
    return value != nullptr && value->kind == Value::Kind::kNumberArray && sizeOf(value->numbers) != 0 ? &value->numbers
                                                                                                       : nullptr;
  };
  const uint8_t tag = integerOf<uint8_t>(node, "type").value_or(uint8_t{1});
  NumberArray* data = filled(kDataArrays[tag < std::size(kDataArrays) ? tag : 1]);
  for (const char* name : kDataArrays) {
    if (data == nullptr) {
      data = filled(name);
    }
  }
  if (data != nullptr) {
    layer.values = std::move(*data);
  }
  return layer;
}

PJ::Expected<NavMapData> extractNavMap(Node& root, size_t max_sparse_bytes) {
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
  if (map.frame_id.empty() && !map.surfaces.empty()) {
    map.frame_id = map.surfaces.front().frame_id;  // as upstream's own conversion does
  }

  if (std::vector<Node>* layers = nodeArrayOf(root, "layers")) {
    std::set<std::string> names;
    size_t sparse_bytes = 0;  // what the kept sparse layers expand to; never above max_sparse_bytes
    for (Node& node : *layers) {
      NavMapLayer layer = extractLayer(node);
      const size_t values = sizeOf(layer.values);
      const bool fits = layer.indices.empty() ? values == navcel_count
                                              : values == layer.indices.size() && allBelow(layer.indices, navcel_count);
      const bool storable = datatypeOf(layer) != Datatype::kUnknown && fillable(layer, infoFor(layer));
      // Division, not multiplication: it cannot overflow.
      const size_t element_size = elementSizeOf(layer.values);
      const bool in_budget = layer.indices.empty() || navcel_count <= (max_sparse_bytes - sparse_bytes) / element_size;
      if (fits && storable && isLayerChannelName(layer.name) && in_budget && names.insert(layer.name).second) {
        if (!layer.indices.empty()) {
          sparse_bytes += navcel_count * element_size;
        }
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
      const auto from = integerOf<uint32_t>(node, "from_navcel");
      const auto to = integerOf<uint32_t>(node, "to_navcel");
      const auto type = node.find("type") != nullptr ? integerOf<uint8_t>(node, "type") : std::optional<uint8_t>(0);
      // A link to a missing NavCel cannot be drawn; skip it rather than lose the map.
      if (!from || !to || !type || *from >= navcel_count || *to >= navcel_count) {
        ++map.dropped_links;
        continue;
      }
      NavMapLink link;
      link.from_navcel = *from;
      link.to_navcel = *to;
      link.type = *type;
      link.bidirectional = numberOf(node, "bidirectional").value_or(0.0) != 0.0;
      // Upstream reads a negative length as "compute it": the cost is unspecified.
      const double length = numberOf(node, "length").value_or(std::numeric_limits<double>::quiet_NaN());
      link.length = length < 0.0 ? std::numeric_limits<double>::quiet_NaN() : length;
      map.links.push_back(link);
    }
  }
  return map;
}

std::optional<int64_t> positiveHeaderStampNs(const Node& root) {
  const Value* header = root.find("header");
  if (header == nullptr || header->kind != Value::Kind::kNode || header->nodes.empty()) {
    return std::nullopt;
  }
  const Value* stamp = header->nodes.front().find("stamp");
  if (stamp == nullptr || stamp->kind != Value::Kind::kNode || stamp->nodes.empty()) {
    return std::nullopt;
  }
  const auto sec = integerOf<int32_t>(stamp->nodes.front(), "sec");
  const auto nanosec = integerOf<uint32_t>(stamp->nodes.front(), "nanosec");
  if (!sec || !nanosec || *nanosec >= 1'000'000'000) {  // ROS requires nanosec < 1 s
    return std::nullopt;
  }
  const int64_t ns = (int64_t{*sec} * 1'000'000'000) + int64_t{*nanosec};
  return ns > 0 ? std::optional<int64_t>(ns) : std::nullopt;
}

PJ::sdk::MeshMap buildMeshMap(NavMapData map, int64_t timestamp_ns) {
  using PJ::sdk::MeshChannel;
  auto owned = std::make_shared<OwnedBlobs>();
  PJ::sdk::MeshMap mesh;
  mesh.timestamp_ns = timestamp_ns;
  mesh.frame_id = std::move(map.frame_id);
  mesh.geometry_id = geometryIdText(map.has_geometry_id, map.geometry_id);
  mesh.vertex_count = static_cast<uint32_t>(map.x.size());
  mesh.face_count = static_cast<uint32_t>(map.v0.size());
  const size_t navcels = map.v0.size();

  // Geometry: NavMap keeps one array per axis and per corner; MeshMap wants them interleaved.
  mesh.position_type = Datatype::kFloat32;
  mesh.positions = owned->add(interleave(map.x, map.y, map.z));
  mesh.indices = owned->add(interleave(map.v0, map.v1, map.v2));

  // Vertex colour: the arrays are already one per channel.
  if (map.has_vertex_rgba) {
    if (map.a.empty()) {
      map.a.assign(map.x.size(), uint8_t{255});
    }
    for (auto& [name, values] :
         {std::pair<const char*, std::vector<uint8_t>*>{"red", &map.r},
          {"green", &map.g},
          {"blue", &map.b},
          {"alpha", &map.a}}) {
      MeshChannel channel;
      channel.name = name;
      channel.target = MeshChannel::Target::kVertex;
      channel.datatype = Datatype::kUint8;
      channel.data = owned->add(std::move(*values));
      mesh.channels.push_back(std::move(channel));
    }
  }

  // The surface each NavCel belongs to, as a categorical channel labelled by
  // frame. A NavCel no surface lists holds the uint32 maximum, declared no-data.
  if (!map.surfaces.empty()) {
    constexpr uint32_t kNoSurface = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> surface(navcels, kNoSurface);
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
    if (std::find(surface.begin(), surface.end(), kNoSurface) != surface.end()) {
      info.no_data = static_cast<double>(kNoSurface);
    }
    mesh.channels.push_back(
        {.name = "surface",
         .target = MeshChannel::Target::kFace,
         .datatype = Datatype::kUint32,
         .data = owned->add(std::move(surface)),
         .info = std::move(info)});
  }

  // One face channel per layer, in the type of its array; a sparse layer is
  // scattered over every NavCel.
  for (NavMapLayer& layer : map.layers) {
    PJ::sdk::ChannelInfo info = infoFor(layer);
    const Datatype datatype = datatypeOf(layer);
    const auto data = addLayerData(*owned, layer.values, layer.indices, navcels, info);
    mesh.channels.push_back(
        {.name = std::move(layer.name),
         .target = MeshChannel::Target::kFace,
         .datatype = datatype,
         .data = data,
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

PJ::Expected<PJ::sdk::MeshMapUpdate> buildMeshMapUpdate(NavMapLayer layer, int64_t timestamp_ns) {
  if (!layer.indices.empty() && layer.indices.size() != sizeOf(layer.values)) {
    return PJ::unexpected(std::string("NavMapLayer: indices and data have different lengths"));
  }
  if (!isLayerChannelName(layer.name)) {
    return PJ::unexpected("NavMapLayer: a map drops a layer named '" + layer.name + "', so there is nothing to update");
  }
  auto owned = std::make_shared<OwnedBlobs>();
  PJ::sdk::MeshMapUpdate update;
  update.timestamp_ns = timestamp_ns;
  update.frame_id = std::move(layer.frame_id);
  update.geometry_id = geometryIdText(layer.has_geometry_id, layer.geometry_id);
  PJ::sdk::ChannelInfo info = infoFor(layer);
  const Datatype datatype = datatypeOf(layer);
  if (datatype == Datatype::kUnknown) {
    return PJ::unexpected(std::string("NavMapLayer: 64-bit integer data cannot be a MeshMap channel"));
  }
  PJ::sdk::MeshChannelPatch patch;
  // The patch lists the values in order; its indices, if any, place them.
  const auto data = addLayerData(*owned, layer.values, {}, 0, info);
  // No indices: a dense patch replacing the whole channel.
  patch.indices = layer.indices.empty() ? PJ::Span<const uint8_t>{} : owned->add(std::move(layer.indices));
  patch.channel = {
      .name = std::move(layer.name),
      .target = PJ::sdk::MeshChannel::Target::kFace,
      .datatype = datatype,
      .data = data,
      .info = std::move(info)};
  update.patches.push_back(std::move(patch));
  update.anchor = std::move(owned);
  return update;
}

}  // namespace navmap_parser
