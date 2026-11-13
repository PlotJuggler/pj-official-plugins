/**
 * @file navmap_parser.cpp
 * @brief EasyNavigation NavMap messages -> sdk::MeshMap / sdk::MeshMapUpdate.
 *
 * Routing: this parser registers SchemaHandlers for exactly two ROS 2 types,
 * so the host's claim resolver prefers it over the generic ROS parser for them
 * on both the object route (3D) and the scalar route (a few summary series
 * instead of flattening million-element arrays):
 *   navmap_ros_interfaces/msg/NavMap      -> kMeshMap
 *   navmap_ros_interfaces/msg/NavMapLayer -> kMeshMapUpdate (the sparse layer
 *                                            updates on `<map topic>_updates`)
 *
 * The host also gives every parser a wildcard scalar claim for each manifest
 * encoding and accepts it whenever bindSchema() succeeds. bindSchema() must
 * therefore REJECT every other type, or this parser would compete with the ROS
 * parser for all ros2msg topics.
 */

#include <cstdint>
#include <exception>
#include <optional>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "navmap_cdr_reader.hpp"
#include "navmap_manifest.hpp"
#include "navmap_mesh.hpp"
#include "navmap_msg_decoder.hpp"

namespace navmap_parser {

namespace {

constexpr std::string_view kNavMapType = "navmap_ros_interfaces/msg/NavMap";
constexpr std::string_view kNavMapLayerType = "navmap_ros_interfaces/msg/NavMapLayer";

// The current navmap_ros_interfaces definitions, used when a topic arrives
// without a schema. Topics that carry their own schema decode by that one.
constexpr std::string_view kLayerDefinition = R"(std_msgs/Header header
string name
uint8 type
string description
string unit
bool has_unknown
float64 unknown_value
uint64 geometry_id
uint32[] indices
uint8[] data_u8
float32[] data_f32
float64[] data_f64
int16[] data_i16
)";

constexpr std::string_view kNavMapDefinition = R"(std_msgs/Header header
float32[] positions_x
float32[] positions_y
float32[] positions_z
bool has_vertex_rgba
uint8[] colors_r
uint8[] colors_g
uint8[] colors_b
uint8[] colors_a
uint32[] navcels_v0
uint32[] navcels_v1
uint32[] navcels_v2
navmap_ros_interfaces/NavMapSurface[] surfaces
navmap_ros_interfaces/NavMapLayer[] layers
uint64 geometry_id
navmap_ros_interfaces/NavMapLink[] links
================================================================================
MSG: navmap_ros_interfaces/NavMapSurface
string frame_id
uint32[] navcels
================================================================================
MSG: navmap_ros_interfaces/NavMapLink
uint32 from_navcel
uint32 to_navcel
uint8 type
float32 length
bool bidirectional
================================================================================
MSG: navmap_ros_interfaces/NavMapLayer
)";

enum class BoundType : uint8_t { kNone, kNavMap, kNavMapLayer };

std::optional<double> numberField(const Node& node, std::string_view name) {
  const Value* value = node.find(name);
  return value != nullptr && value->kind == Value::Kind::kNumber ? std::optional<double>(value->number) : std::nullopt;
}

}  // namespace

class NavMapParser : public PJ::MessageParserPluginBase {
 public:
  NavMapParser() {
    PJ::sdk::SchemaHandler map_handler{
        .object_type = PJ::sdk::BuiltinObjectType::kMeshMap,
        .parse_scalars = [this](
                             PJ::Timestamp ts, PJ::Span<const uint8_t> payload) { return decodeScalars(ts, payload); },
        .parse_object = [this](PJ::Timestamp ts, PJ::sdk::PayloadView payload) { return decodeMap(ts, payload); },
    };
    PJ::sdk::SchemaHandler layer_handler{
        .object_type = PJ::sdk::BuiltinObjectType::kMeshMapUpdate,
        .parse_scalars = [this](
                             PJ::Timestamp ts, PJ::Span<const uint8_t> payload) { return decodeScalars(ts, payload); },
        .parse_object = [this](PJ::Timestamp ts, PJ::sdk::PayloadView payload) { return decodeLayer(ts, payload); },
    };
    // Hosts pass the normalized "pkg/msg/Type"; accept the short form too.
    registerSchemaHandler(kNavMapType, map_handler);
    registerSchemaHandler(normalizeRosTypeName(kNavMapType), map_handler);
    registerSchemaHandler(kNavMapLayerType, layer_handler);
    registerSchemaHandler(normalizeRosTypeName(kNavMapLayerType), layer_handler);
  }

  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override {
    schema_.reset();
    bound_ = BoundType::kNone;
    const std::string normalized = normalizeRosTypeName(type_name);
    std::string_view root;
    std::string fallback;
    if (normalized == normalizeRosTypeName(kNavMapType)) {
      bound_ = BoundType::kNavMap;
      root = kNavMapType;
      fallback = std::string(kNavMapDefinition) + std::string(kLayerDefinition);
    } else if (normalized == normalizeRosTypeName(kNavMapLayerType)) {
      bound_ = BoundType::kNavMapLayer;
      root = kNavMapLayerType;
      fallback = std::string(kLayerDefinition);
    } else {
      return PJ::unexpected(
          "NavMap parser only decodes " + std::string(kNavMapType) + " and " + std::string(kNavMapLayerType) +
          ", not " + std::string(type_name));
    }
    if (auto status = PJ::MessageParserPluginBase::bindSchema(type_name, schema); !status) {
      return status;
    }
    std::string_view definition(reinterpret_cast<const char*>(schema.data()), schema.size());
    if (definition.find_first_not_of(" \t\r\n") == std::string_view::npos) {
      definition = fallback;
    }
    auto compiled = compileMsgBundle(root, definition);
    if (!compiled) {
      return PJ::unexpected("NavMap schema: " + compiled.error());
    }
    schema_ = std::move(*compiled);
    return PJ::okStatus();
  }

 private:
  // One summary row per message; the arrays are counted, never copied.
  PJ::Expected<PJ::sdk::ScalarRecord> decodeScalars(PJ::Timestamp /*ts*/, PJ::Span<const uint8_t> payload) {
    if (!schema_) {
      return PJ::unexpected(std::string("NavMap parser has no bound schema"));
    }
    try {
      CdrReader reader(payload.data(), payload.size());
      const Node root = decodeMessage(*schema_, reader, /*keep_primitive_arrays=*/false);

      PJ::sdk::ScalarRecord record;
      auto count = [&](std::string_view name) -> uint32_t {
        const Value* value = root.find(name);
        return value != nullptr ? value->count : 0U;
      };
      if (const Value* header = root.find("header"); header != nullptr && !header->nodes.empty()) {
        const Node& h = header->nodes.front();
        if (const Value* stamp = h.find("stamp"); stamp != nullptr && !stamp->nodes.empty()) {
          const double sec = numberField(stamp->nodes.front(), "sec").value_or(0.0);
          const double nanosec = numberField(stamp->nodes.front(), "nanosec").value_or(0.0);
          record.fields.push_back({"/header/stamp", PJ::sdk::ValueRef{sec + (nanosec * 1e-9)}});
        }
        if (const Value* frame = h.find("frame_id"); frame != nullptr) {
          frame_id_ = frame->text;
          record.fields.push_back({"/header/frame_id", PJ::sdk::ValueRef{std::string_view(frame_id_)}});
        }
      }
      if (bound_ == BoundType::kNavMap) {
        record.fields.push_back({"/num_vertices", PJ::sdk::ValueRef{count("positions_x")}});
        record.fields.push_back({"/num_navcels", PJ::sdk::ValueRef{count("navcels_v0")}});
        record.fields.push_back({"/num_surfaces", PJ::sdk::ValueRef{count("surfaces")}});
        record.fields.push_back({"/num_layers", PJ::sdk::ValueRef{count("layers")}});
        if (const Value* has_rgba = root.find("has_vertex_rgba")) {
          record.fields.push_back({"/has_vertex_rgba", PJ::sdk::ValueRef{has_rgba->number != 0.0}});
        }
        if (root.find("links") != nullptr) {
          record.fields.push_back({"/num_links", PJ::sdk::ValueRef{count("links")}});
        }
      } else {
        if (const Value* name = root.find("name"); name != nullptr) {
          layer_name_ = name->text;
          record.fields.push_back({"/name", PJ::sdk::ValueRef{std::string_view(layer_name_)}});
        }
        const uint32_t values = std::max({count("data_u8"), count("data_f32"), count("data_f64"), count("data_i16")});
        record.fields.push_back({"/num_values", PJ::sdk::ValueRef{values}});
        record.fields.push_back({"/sparse", PJ::sdk::ValueRef{count("indices") != 0}});
      }
      if (const Value* geometry_id = root.find("geometry_id")) {
        record.fields.push_back({"/geometry_id", PJ::sdk::ValueRef{geometry_id->unsigned_bits}});
      }
      return record;
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("NavMap: ") + e.what());
    }
  }

  // The object carries the host's message time (std::nullopt), like the ROS
  // parser's default, so the map aligns with the other topics.
  PJ::Expected<PJ::sdk::ObjectRecord> decodeMap(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
    if (!schema_) {
      return PJ::unexpected(std::string("NavMap parser has no bound schema"));
    }
    try {
      CdrReader reader(payload.bytes.data(), payload.bytes.size());
      Node root = decodeMessage(*schema_, reader, /*keep_primitive_arrays=*/true);
      auto map = extractNavMap(root);
      if (!map) {
        return PJ::unexpected(map.error());
      }
      return PJ::sdk::ObjectRecord{.ts = std::nullopt, .object = PJ::sdk::BuiltinObject{buildMeshMap(*map, ts)}};
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("NavMap: ") + e.what());
    }
  }

  PJ::Expected<PJ::sdk::ObjectRecord> decodeLayer(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
    if (!schema_) {
      return PJ::unexpected(std::string("NavMap parser has no bound schema"));
    }
    try {
      CdrReader reader(payload.bytes.data(), payload.bytes.size());
      Node root = decodeMessage(*schema_, reader, /*keep_primitive_arrays=*/true);
      auto update = buildMeshMapUpdate(extractLayer(root), ts);
      if (!update) {
        return PJ::unexpected(update.error());
      }
      return PJ::sdk::ObjectRecord{.ts = std::nullopt, .object = PJ::sdk::BuiltinObject{std::move(*update)}};
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("NavMap: ") + e.what());
    }
  }

  std::optional<MsgSchema> schema_;
  BoundType bound_ = BoundType::kNone;
  std::string frame_id_;    ///< Backs the frame_id string_view of the last scalar record.
  std::string layer_name_;  ///< Backs the name string_view of the last scalar record.
};

}  // namespace navmap_parser

PJ_MESSAGE_PARSER_PLUGIN_NAMED(navmap_parser::NavMapParser, NavMapParser, kNavMapManifest)
