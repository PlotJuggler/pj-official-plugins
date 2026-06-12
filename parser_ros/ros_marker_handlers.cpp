/**
 * @file ros_marker_handlers.cpp
 * @brief visualization_msgs/Marker(Array) -> sdk::SceneEntities (per-message,
 *        stateless decode). See parser_ros/MARKER_NOTES.md for the full design
 *        rationale (why statefulness is NOT resolved here, what is deferred).
 *
 * Each Marker becomes one SceneEntity (ADD/MODIFY) or one SceneEntityDeletion
 * (DELETE/DELETEALL). The decode is positional CDR — the Marker wire layout is
 * identical across all supported ROS 2 distros (humble..rolling); the only
 * divergence is the `texture_resource`/`texture`/`uv_coordinates` block and the
 * `mesh_file` field (absent in EOL foxy/galactic and in ROS 1). bindSchema
 * sniffs the bound .msg definition and sets marker_has_texture_block_ /
 * marker_has_mesh_file_ so the variable tail is consumed correctly on every
 * layout — which keeps each marker in a MarkerArray aligned for the next.
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ros_parser_internal.hpp"

namespace ros_parser_detail {

namespace {

// visualization_msgs/Marker.type constants.
namespace marker_type {
constexpr int32_t kArrow = 0;
constexpr int32_t kCube = 1;
constexpr int32_t kSphere = 2;
constexpr int32_t kCylinder = 3;
constexpr int32_t kLineStrip = 4;
constexpr int32_t kLineList = 5;
constexpr int32_t kCubeList = 6;
constexpr int32_t kSphereList = 7;
constexpr int32_t kTextViewFacing = 9;
constexpr int32_t kMeshResource = 10;
constexpr int32_t kTriangleList = 11;
}  // namespace marker_type

// visualization_msgs/Marker.action constants (ADD == MODIFY == 0).
namespace marker_action {
constexpr int32_t kDelete = 2;
constexpr int32_t kDeleteAll = 3;
}  // namespace marker_action

// Reject pathological array sizes from corrupt/hostile payloads before allocating.
constexpr uint32_t kMaxMarkerVertices = 2'000'000;
constexpr uint32_t kMaxMarkersPerArray = 200'000;

// visualization_msgs/MeshFile carries only a filename (no MIME type), so the
// media type of an inline mesh is inferred from the extension. The returned
// strings match what the Scene3D consumer's hintFromMediaType() recognizes; an
// unknown extension becomes "model/<ext>" so the consumer can still recover a
// format from the suffix. An extensionless filename yields an empty media_type.
std::string mediaTypeFromMeshFilename(const std::string& filename) {
  const auto dot = filename.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= filename.size()) {
    return {};
  }
  std::string ext = filename.substr(dot + 1);
  std::transform(
      ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (ext == "glb") {
    return "model/gltf-binary";
  }
  if (ext == "gltf") {
    return "model/gltf+json";
  }
  if (ext == "dae") {
    return "model/vnd.collada+xml";
  }
  if (ext == "stl") {
    return "model/stl";
  }
  if (ext == "obj") {
    return "model/obj";
  }
  return "model/" + ext;
}

uint8_t readU8(RosMsgParser::Deserializer& d) {
  return d.deserialize(RosMsgParser::UINT8).extract<uint8_t>();
}

uint8_t toByteColor(float c) {
  return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
}

// Unambiguous (ns, id) -> entity id. ns is length-prefixed so distinct pairs
// can never alias (e.g. ns="a/b",id=1 vs ns="a" with an id rendered "b/1").
std::string makeEntityId(const std::string& ns, int32_t id) {
  return std::to_string(ns.size()) + ":" + ns + ":" + std::to_string(id);
}

}  // namespace

void RosParser::decodeOneMarker(PJ::sdk::SceneEntities& out) {
  auto rf64 = [&] { return deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>(); };
  auto rf32 = [&] { return deserializer_->deserialize(RosMsgParser::FLOAT32).convert<float>(); };
  auto ri32 = [&] { return deserializer_->deserialize(RosMsgParser::INT32).convert<int32_t>(); };

  const HeaderData header = readHeader();
  std::string ns;
  deserializer_->deserializeString(ns);
  const int32_t id = ri32();
  const int32_t type = ri32();
  const int32_t action = ri32();

  PJ::sdk::Pose pose;
  pose.position = {.x = rf64(), .y = rf64(), .z = rf64()};
  pose.orientation = {.x = rf64(), .y = rf64(), .z = rf64(), .w = rf64()};

  const PJ::sdk::Vector3 scale{.x = rf64(), .y = rf64(), .z = rf64()};

  const PJ::sdk::ColorRGBA color{toByteColor(rf32()), toByteColor(rf32()), toByteColor(rf32()), toByteColor(rf32())};

  // lifetime: builtin_interfaces/Duration (int32 sec, uint32 nanosec).
  const int32_t life_sec = ri32();
  const uint32_t life_nsec = deserializer_->deserializeUInt32();
  const bool frame_locked = readU8(*deserializer_) != 0;

  const uint32_t num_points = deserializer_->deserializeUInt32();
  if (num_points > kMaxMarkerVertices) {
    throw std::runtime_error("Marker points[] exceeds sanity cap");
  }
  std::vector<PJ::sdk::Point3> points;
  points.reserve(num_points);
  for (uint32_t i = 0; i < num_points; ++i) {
    points.push_back({.x = rf64(), .y = rf64(), .z = rf64()});
  }

  const uint32_t num_colors = deserializer_->deserializeUInt32();
  if (num_colors > kMaxMarkerVertices) {
    throw std::runtime_error("Marker colors[] exceeds sanity cap");
  }
  std::vector<PJ::sdk::ColorRGBA> colors;
  colors.reserve(num_colors);
  for (uint32_t i = 0; i < num_colors; ++i) {
    colors.push_back({toByteColor(rf32()), toByteColor(rf32()), toByteColor(rf32()), toByteColor(rf32())});
  }

  // --- Variable tail. Always consumed in full so the next marker in a
  // MarkerArray stays aligned, even when its fields are unused. ---
  if (marker_has_texture_block_) {
    std::string texture_resource;
    deserializer_->deserializeString(texture_resource);
    // texture: sensor_msgs/CompressedImage. Skip its header inline rather than
    // via readHeader() (which would clobber current_timestamp_). The block only
    // exists in ROS 2, but branch on isROS2() defensively.
    if (!deserializer_->isROS2()) {
      (void)deserializer_->deserializeUInt32();  // header.seq (ROS 1 only)
    }
    (void)deserializer_->deserializeUInt32();  // header.stamp.sec
    (void)deserializer_->deserializeUInt32();  // header.stamp.nsec
    std::string texture_frame;
    deserializer_->deserializeString(texture_frame);
    std::string texture_format;
    deserializer_->deserializeString(texture_format);
    (void)deserializer_->deserializeByteSequence();  // texture.data
    const uint32_t num_uv = deserializer_->deserializeUInt32();
    if (num_uv > kMaxMarkerVertices) {
      throw std::runtime_error("Marker uv_coordinates[] exceeds sanity cap");
    }
    for (uint32_t i = 0; i < num_uv; ++i) {
      rf32();  // u
      rf32();  // v
    }
  }

  std::string text;
  deserializer_->deserializeString(text);
  std::string mesh_resource;
  deserializer_->deserializeString(mesh_resource);
  std::string mesh_filename;
  std::vector<uint8_t> mesh_file_data;
  if (marker_has_mesh_file_) {
    deserializer_->deserializeString(mesh_filename);
    // The byte sequence must be consumed unconditionally to keep the wire
    // aligned, but only a MESH_RESOURCE marker uses it — skip the copy of a
    // potentially multi-MB payload for every other marker type.
    const auto bytes = deserializer_->deserializeByteSequence();  // mesh_file.data
    if (type == marker_type::kMeshResource) {
      mesh_file_data.assign(bytes.begin(), bytes.end());
    }
  }
  const bool mesh_use_embedded = readU8(*deserializer_) != 0;

  const PJ::Timestamp stamp_ns =
      static_cast<PJ::Timestamp>(static_cast<int64_t>(header.sec) * 1000000000LL + static_cast<int64_t>(header.nsec));

  if (action == marker_action::kDelete) {
    out.deletions.push_back(
        PJ::sdk::SceneEntityDeletion{
            .type = PJ::sdk::SceneEntityDeletion::Type::kMatchingId,
            .timestamp = stamp_ns,
            .id = makeEntityId(ns, id)});
    return;
  }
  if (action == marker_action::kDeleteAll) {
    out.deletions.push_back(
        PJ::sdk::SceneEntityDeletion{
            .type = PJ::sdk::SceneEntityDeletion::Type::kAll, .timestamp = stamp_ns, .id = std::string()});
    return;
  }

  // ADD / MODIFY. Per-vertex colors only when they match the vertex count
  // (ROS does not enforce the equality); otherwise fall back to the solid color.
  const bool use_vertex_colors = !colors.empty() && colors.size() == points.size();

  PJ::sdk::SceneEntity entity;
  entity.timestamp = stamp_ns;
  entity.frame_id = header.frame_id;
  entity.id = makeEntityId(ns, id);
  entity.lifetime_ns = static_cast<int64_t>(life_sec) * 1000000000LL + static_cast<int64_t>(life_nsec);
  entity.frame_locked = frame_locked;

  switch (type) {
    case marker_type::kArrow: {
      // Approximate: ROS encodes an arrow either by scale (x=length) or by two
      // points (start, end). SceneEntity's ArrowPrimitive wants shaft/head dims.
      PJ::sdk::ArrowPrimitive arrow;
      arrow.color = color;
      arrow.shaft_diameter = scale.x;
      arrow.head_diameter = scale.y > 0.0 ? scale.y : scale.x;
      double length = scale.x;
      if (points.size() >= 2) {
        const auto& a = points[0];
        const auto& b = points[1];
        length = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z));
        arrow.pose.position = {.x = a.x, .y = a.y, .z = a.z};
      } else {
        arrow.pose = pose;
      }
      arrow.shaft_length = length * 0.8;
      arrow.head_length = length * 0.2;
      entity.arrows.push_back(arrow);
      break;
    }
    case marker_type::kCube:
      entity.cubes.push_back({.pose = pose, .size = scale, .color = color});
      break;
    case marker_type::kSphere:
      entity.spheres.push_back({.pose = pose, .size = scale, .color = color});
      break;
    case marker_type::kCylinder:
      entity.cylinders.push_back({.pose = pose, .size = scale, .bottom_scale = 1.0, .top_scale = 1.0, .color = color});
      break;
    case marker_type::kLineStrip:
    case marker_type::kLineList: {
      PJ::sdk::LinePrimitive line;
      line.type = (type == marker_type::kLineStrip) ? PJ::sdk::LineType::kLineStrip : PJ::sdk::LineType::kLineList;
      line.pose = pose;
      line.thickness = scale.x;
      line.points = points;
      line.color = color;
      if (use_vertex_colors) {
        line.colors = colors;
      }
      entity.lines.push_back(std::move(line));
      break;
    }
    case marker_type::kTriangleList: {
      PJ::sdk::TrianglePrimitive tri;
      tri.pose = pose;
      tri.points = points;
      tri.color = color;
      if (use_vertex_colors) {
        tri.colors = colors;
      }
      entity.triangles.push_back(std::move(tri));
      break;
    }
    case marker_type::kCubeList:
    case marker_type::kSphereList: {
      // Expand to one primitive per point. Each glyph sits at its point in the
      // marker frame; marker-pose translation is applied, full rotation
      // composition is deferred (correct for identity / translation-only poses).
      for (size_t i = 0; i < points.size(); ++i) {
        PJ::sdk::Pose glyph_pose;
        glyph_pose.position = {
            .x = pose.position.x + points[i].x, .y = pose.position.y + points[i].y, .z = pose.position.z + points[i].z};
        glyph_pose.orientation = pose.orientation;
        const PJ::sdk::ColorRGBA c = use_vertex_colors ? colors[i] : color;
        if (type == marker_type::kCubeList) {
          entity.cubes.push_back({.pose = glyph_pose, .size = scale, .color = c});
        } else {
          entity.spheres.push_back({.pose = glyph_pose, .size = scale, .color = c});
        }
      }
      break;
    }
    case marker_type::kTextViewFacing: {
      PJ::sdk::TextPrimitive t;
      t.pose = pose;
      t.billboard = true;
      t.font_size = scale.z;
      t.color = color;
      t.text = std::move(text);
      entity.texts.push_back(std::move(t));
      break;
    }
    case marker_type::kMeshResource: {
      if (mesh_resource.empty() && mesh_file_data.empty()) {
        return;  // neither a URL nor inline bytes: no asset to reference
      }
      PJ::sdk::ModelPrimitive model;
      model.pose = pose;
      model.scale = scale;
      model.color = color;
      model.override_color = !mesh_use_embedded;
      if (!mesh_file_data.empty()) {  // inline mesh (ROS 2 humble+ mesh_file)
        model.data = std::move(mesh_file_data);
        model.media_type = mediaTypeFromMeshFilename(mesh_filename);
      }
      if (!mesh_resource.empty()) {  // keep the publisher's URL/identifier too
        model.url = std::move(mesh_resource);
      }
      entity.models.push_back(std::move(model));
      break;
    }
    default:
      // POINTS (8), ARROW_STRIP (12), or unknown: no SceneEntity primitive.
      // POINTS belongs in kPointCloud; the rest have no canonical mapping yet.
      return;
  }

  out.entities.push_back(std::move(entity));
}

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseMarker(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    PJ::sdk::SceneEntities out;
    decodeOneMarker(out);
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(out)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("Marker: CDR read error: ") + e.what());
  }
}

PJ::Expected<PJ::sdk::ObjectRecord> RosParser::parseMarkerArray(PJ::Timestamp ts, PJ::sdk::PayloadView payload) {
  try {
    ensureDeserializer();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.bytes.data(), payload.bytes.size()));
    const uint32_t count = deserializer_->deserializeUInt32();
    if (count > kMaxMarkersPerArray) {
      return PJ::unexpected(std::string("MarkerArray: too many markers"));
    }
    PJ::sdk::SceneEntities out;
    out.entities.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      decodeOneMarker(out);
    }
    return PJ::sdk::ObjectRecord{
        .ts = use_embedded_timestamp_ ? std::optional<PJ::Timestamp>{current_timestamp_} : std::nullopt,
        .object = PJ::sdk::BuiltinObject{std::move(out)}};
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("MarkerArray: CDR read error: ") + e.what());
  }
}

}  // namespace ros_parser_detail
