#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ros_parser_internal.hpp"

namespace ros_parser_detail {

namespace {

// Port of the legacy PJ::ParseDouble helper (PJ3
// `plotjuggler-ros-plugins/src/parser_configuration.cpp:115-153`). Same
// three-stage semantics, PJ::parseNumber (locale-independent, like the
// original boost::spirit) in place of boost::spirit:
//
//   1. Strict full-string double parse: success only when the entire `str`
//      is a valid number.
//   2. If that fails and `remove_suffix` is on, scan a numeric prefix
//      consisting of {digit, '+', '-', '.'} characters, stop at the first
//      character outside that set, and re-parse the prefix. PJ3 quirks are
//      preserved verbatim — scientific-notation 'e' is treated as a
//      non-numeric terminator, so "1e3km" yields 1 not 1000.
//   3. If still unparsed and `parse_boolean` is on, match a case-insensitive
//      "true"/"false" of length 4-5 to 1.0/0.0.
//
// Returns true and writes to `value` on success; returns false (leaving
// `value` unchanged from the caller's perspective) otherwise.
bool parseStringAsDouble(const std::string& str, double& value, bool remove_suffix, bool parse_boolean) {
  if (str.empty()) {
    return false;
  }

  // PJ::parseNumber keeps the strict whole-string semantics of the previous
  // strtod call but ignores LC_NUMERIC, so "3.14" parses under comma-decimal
  // locales too (strtod stopped at the '.' and failed the full-consumption
  // check there).
  if (auto parsed = PJ::parseNumber<double>(str)) {
    value = *parsed;
    return true;
  }

  if (remove_suffix) {
    std::size_t pos = 0;
    while (pos < str.size()) {
      const char c = str[pos];
      if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '+' && c != '.') {
        break;
      }
      ++pos;
    }
    if (pos > 0 && pos < str.size()) {
      if (auto parsed = PJ::parseNumber<double>(std::string_view(str).substr(0, pos))) {
        value = *parsed;
        return true;
      }
    }
  }

  if (parse_boolean && str.size() >= 4 && str.size() <= 5) {
    const std::string lower = PJ::sdk::lowerAscii(str);
    if (lower == "true") {
      value = 1.0;
      return true;
    }
    if (lower == "false") {
      value = 0.0;
      return true;
    }
  }

  return false;
}

std::string normalizedMessageType(std::string_view type_name, RosMsgParser::SchemaFormat schema_format) {
  std::string msg_type(type_name);
  if (schema_format == RosMsgParser::DDS_IDL) {
    // ROS2 IDL uses package resource names; OMG IDL uses scoped names.
    // rosx_introspection keeps module separators as "::" and the last as "/".
    for (size_t pos = 0; (pos = msg_type.find('/', pos)) != std::string::npos; pos += 2) {
      msg_type.replace(pos, 1, "::");
    }
    if (auto pos = msg_type.rfind("::"); pos != std::string::npos) {
      msg_type.replace(pos, 2, "/");
    }
  } else {
    for (size_t pos = 0; (pos = msg_type.find("::", pos)) != std::string::npos; ++pos) {
      msg_type.replace(pos, 2, "/");
    }
    if (auto pos = msg_type.find("/msg/"); pos != std::string::npos) {
      msg_type.erase(pos, 4);
    }
  }
  return msg_type;
}

PJ::Expected<std::pair<std::string, RosMsgParser::SchemaFormat>> schemaEncodingToFormat(std::string_view encoding) {
  if (encoding == "omgidl" || encoding == "ros2idl") {
    return std::make_pair(std::string(encoding), RosMsgParser::DDS_IDL);
  }
  if (encoding == "ros1msg" || encoding == "ros1") {
    return std::make_pair(std::string("ros1msg"), RosMsgParser::ROS_MSG);
  }
  if (encoding.empty() || encoding == "ros2msg" || encoding == "ros2") {
    return std::make_pair(std::string("ros2msg"), RosMsgParser::ROS_MSG);
  }
  return PJ::unexpected(std::string("unsupported ROS schema encoding: ") + std::string(encoding));
}

// MCAP ros2idl bundles named files. Expand only the root and its includes;
// other preprocessor directives are ignored by the format's limited preprocessor.
std::string ros2IdlDefinition(const std::string& root_name, const std::string& bundle) {
  const std::string separator(80, '=');
  std::map<std::string, std::string> files;
  std::istringstream input(bundle);
  std::string line;
  std::string file_name;
  while (std::getline(input, line)) {
    if (line.ends_with('\r')) {
      line.pop_back();
    }
    if (line == separator) {
      if (!std::getline(input, line) || !line.starts_with("IDL: ")) {
        throw std::runtime_error("ros2idl section is missing its IDL file name");
      }
      if (line.ends_with('\r')) {
        line.pop_back();
      }
      file_name = line.substr(5);
      if (file_name.empty() || !files.emplace(file_name, std::string{}).second) {
        throw std::runtime_error("empty or duplicate ros2idl file name: " + file_name);
      }
    } else {
      if (file_name.empty()) {
        throw std::runtime_error("ros2idl content precedes its first section");
      }
      files.at(file_name) += line + '\n';
    }
  }

  static const std::regex include_pattern(R"rx(^\s*#\s*include\s*(?:"([^"]+)\.idl"|<([^>]+)\.idl>)\s*(?://.*)?$)rx");
  std::vector<std::istringstream> sources;
  std::set<std::string> included;
  std::string definition;
  const auto include_file = [&](const std::string& name) {
    if (!included.insert(name).second) {
      return;
    }
    const auto file = files.find(name);
    if (file == files.end()) {
      throw std::runtime_error("missing ros2idl file: " + name);
    }
    sources.emplace_back(file->second);
  };
  include_file(root_name);
  while (!sources.empty()) {
    if (!std::getline(sources.back(), line)) {
      sources.pop_back();
      continue;
    }
    std::smatch match;
    if (std::regex_match(line, match, include_pattern)) {
      include_file(match[1].matched ? match[1].str() : match[2].str());
    } else if (const auto first = line.find_first_not_of(" \t"); first == std::string::npos || line[first] != '#') {
      definition += line + '\n';
    }
  }
  return definition;
}

}  // namespace

// ---------------------------------------------------------------------------
// Class-level catalog of every ROS schema this parser recognizes.
//
// What the catalog IS: pure data — pointers to member functions, no `this`
// capture, populated once per process via the static-local map. Adding a
// schema = one new entry here.
//
// What an INSTANCE does with it: bindSchema looks up the entry for the
// bound type and registers a single SchemaHandler tailored to it on this
// instance. If the bound type is not in the catalog, bindSchema registers
// a generic flatten handler under the bound type name. Either way, the
// per-instance handlers_ table ends up with exactly one entry — honest
// about the fact that one RosParser instance binds to one schema.
//
// Field naming mirrors PJ::sdk::SchemaHandler: object_type / parse_scalars /
// parse_object. Type difference for parse_scalars (raw void member fn vs.
// the std::function the SchemaHandler expects) is intentional — bindSchema
// adapts via wrapVoidHandler at registration time.
// ---------------------------------------------------------------------------

const std::unordered_map<std::string, RosParser::CatalogEntry>& RosParser::catalog() {
  using ObjectType = PJ::sdk::BuiltinObjectType;
  static const std::unordered_map<std::string, CatalogEntry> kMap = {
      // ----- Builtin-object schemas -----
      {"sensor_msgs/Image",
       {.object_type = ObjectType::kImage,
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
        .parse_object = &RosParser::parseImage}},
      {"sensor_msgs/CompressedImage",
       {.object_type = ObjectType::kImage,  // unified Image distinguishes by encoding string.
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
        .parse_object = &RosParser::parseCompressedImage}},
      // CameraInfo is consumed as a calibration object (intrinsics + distortion)
      // so the 2D image view can rectify frames and line up annotation overlays;
      // the consumer pairs it with the image by frame_id. The scalar route keeps
      // the small matrices (K/D/R/P, width, height) plottable.
      {"sensor_msgs/CameraInfo",
       {.object_type = ObjectType::kCameraInfo,
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
        .parse_object = &RosParser::parseCameraInfo}},
      // foxglove_msgs/CompressedVideo — a single compressed video frame
      // (h264/h265/vp9/av1). The scalar route is included so the object-ingest
      // path runs (an object-only entry would abort the push before the object
      // route fires); it discards the large data[] blob and keeps frame_id /
      // format as plottable columns, mirroring the Image entries.
      {"foxglove_msgs/CompressedVideo",
       {.object_type = ObjectType::kVideoFrame,
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
        .parse_object = &RosParser::parseCompressedVideo}},
      // A promoted cloud is consumed as a 3D object; its scalar route exists to
      // keep the message plottable on a time axis, not to mirror the message.
      // parseScalarsDiscardingLargeArrays would run a full introspection walk
      // per message to yield mostly per-PointField noise, so the route is a slim
      // hand-written read of the header + the measured point count instead.
      {"sensor_msgs/PointCloud2",
       {.object_type = ObjectType::kPointCloud,
        .parse_scalars = &RosParser::parsePointCloud2Scalars,
        .parse_object = &RosParser::parsePointCloud}},
      // sensor_msgs/LaserScan — eagerly projected to a canonical PointCloud via
      // the shared pj_laser_scan projector (cos/sin LUT cached per scanner
      // config, so the trig runs once per recording). The scalar route keeps
      // the pre-promotion generic flatten: angle_*/ranges[i] columns still
      // appear, subject to the user-configured array policy.
      {"sensor_msgs/LaserScan",
       {.object_type = ObjectType::kPointCloud,
        .parse_scalars = &RosParser::parseScalarsGeneric,
        .parse_object = &RosParser::parseLaserScan}},
      // Log messages feed the log viewer as sdk::Log objects; the scalar route
      // keeps the generic flatten so level/line/... stay plottable. Keys are
      // normalised ("/msg/" stripped), so rcl_interfaces/msg/Log matches too.
      {"rcl_interfaces/Log",
       {.object_type = ObjectType::kLog,
        .parse_scalars = &RosParser::parseScalarsGeneric,
        .parse_object = &RosParser::parseLogObject}},
      {"rosgraph_msgs/Log",
       {.object_type = ObjectType::kLog,
        .parse_scalars = &RosParser::parseScalarsGeneric,
        .parse_object = &RosParser::parseLogObject}},
      // Other log-equivalent messages (they carry a severity), same dual route.
      {"foxglove_msgs/Log",
       {.object_type = ObjectType::kLog,
        .parse_scalars = &RosParser::parseScalarsGeneric,
        .parse_object = &RosParser::parseFoxgloveLogObject}},
      {"mavros_msgs/StatusText",
       {.object_type = ObjectType::kLog,
        .parse_scalars = &RosParser::parseScalarsGeneric,
        .parse_object = &RosParser::parseStatusTextLogObject}},
      {"px4_msgs/LogMessage",
       {.object_type = ObjectType::kLog,
        .parse_scalars = &RosParser::parseScalarsGeneric,
        .parse_object = &RosParser::parsePx4LogMessageObject}},
      {"px4_msgs/MavlinkLog",
       {.object_type = ObjectType::kLog,
        .parse_scalars = &RosParser::parseScalarsGeneric,
        .parse_object = &RosParser::parsePx4MavlinkLogObject}},
      // foxglove_msgs/CompressedPointCloud — opaque compressed cloud (draco/cloudini/…).
      // The parser repackages the blob + format; it does not decode. Its scalar
      // route reads the BARE builtin_interfaces/Time head (this schema has no
      // std_msgs/Header) and emits /timestamp + /frame_id; the compressed wire
      // format carries no point count.
      {"foxglove_msgs/CompressedPointCloud",
       {.object_type = ObjectType::kCompressedPointCloud,
        .parse_scalars = &RosParser::parseFoxgloveCompressedPointCloudScalars,
        .parse_object = &RosParser::parseFoxgloveCompressedPointCloud}},
      // point_cloud_interfaces/CompressedPointCloud2 — the point_cloud_transport
      // canonical compressed message. Header-only scalar route: the payload is a
      // compressed blob, so no point count can be MEASURED from it (compressed
      // size / point_step is meaningless), and the declared width * height would
      // be a claim, not a measurement.
      {"point_cloud_interfaces/CompressedPointCloud2",
       {.object_type = ObjectType::kCompressedPointCloud,
        .parse_scalars = &RosParser::parseHeaderOnlyScalars,
        .parse_object = &RosParser::parseCompressedPointCloud2}},
      // TF keeps its specialized scalar flattening (handleTFMessage) AND emits a
      // canonical FrameTransforms object for the 3D scene's TF buffer.
      {"tf2_msgs/TFMessage",
       {.object_type = ObjectType::kFrameTransforms,
        .parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleTFMessage>,
        .parse_object = &RosParser::parseFrameTransforms}},
      // A map/costmap is consumed as a grid object; parseScalarsDiscardingLargeArrays
      // keeps the metadata (resolution, size, origin) plottable while discarding the
      // large data[] array. The scalar handler is also required by the ingest path —
      // an object-only entry (no parse_scalars) aborts the message push before the
      // object route runs, so nothing reaches the ObjectStore.
      {"nav_msgs/OccupancyGrid",
       {.object_type = ObjectType::kOccupancyGrid,
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
        .parse_object = &RosParser::parseOccupancyGrid}},
      // The incremental delta-patch counterpart to OccupancyGrid (e.g.
      // costmap_updates). Same dual route: x/y/width/height stay plottable, the
      // large data[] patch is discarded by the scalar handler.
      {"map_msgs/OccupancyGridUpdate",
       {.object_type = ObjectType::kOccupancyGridUpdate,
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
        .parse_object = &RosParser::parseOccupancyGridUpdate}},
      // grid_map_msgs/GridMap (elevation maps, layered costmaps) becomes a
      // canonical GridMap through the pj_grid_map transcoder. The slim scalar
      // route keeps the header + GridMapInfo + layer/size counts plottable
      // without walking the per-layer float arrays.
      {"grid_map_msgs/GridMap",
       {.object_type = ObjectType::kGridMap,
        .parse_scalars = &RosParser::parseGridMapScalars,
        .parse_object = &RosParser::parseGridMap}},
      // Foxglove's own grid message shares the canonical packed cell layout, so
      // data[] is a zero-copy view. Bare-time head (no std_msgs/Header).
      {"foxglove_msgs/Grid",
       {.object_type = ObjectType::kGridMap,
        .parse_scalars = &RosParser::parseFoxgloveGridScalars,
        .parse_object = &RosParser::parseFoxgloveGrid}},
      // Markers are 3D scene content with no meaningful scalar columns, but they
      // still need a slim parse_scalars: an object-only entry makes the host's
      // eager-scalar ingest abort the push on any non-kPureLazy policy (e.g. live
      // streams, which set no override), silently dropping the object.
      // parseScalarsObjectOnly returns an empty row so the SceneEntities object
      // ingests under ANY policy. One SceneEntity per Marker (ADD/MODIFY) or a
      // SceneEntityDeletion (DELETE/DELETEALL) — see MARKER_NOTES.md.
      //
      // A single Marker DOES lead with a std_msgs/Header, so its scalar route
      // reads it and emits the header series (a MarkerArray has no top-level
      // header — only its elements do — and keeps the empty route).
      {"visualization_msgs/Marker",
       {.object_type = ObjectType::kSceneEntities,
        .parse_scalars = &RosParser::parseHeaderOnlyScalars,
        .parse_object = &RosParser::parseMarker}},
      {"visualization_msgs/MarkerArray",
       {.object_type = ObjectType::kSceneEntities,
        .parse_scalars = &RosParser::parseScalarsObjectOnly,
        .parse_object = &RosParser::parseMarkerArray}},
      // YOLO detections become 2D image overlays (boxes + labels + mask outline +
      // keypoints). The slim parse_scalars emits num_detections so the overlay
      // ingests under any policy (same ingest reason as the markers above) and
      // yields a plottable count. Third-party message, net-new — see YOLO_NOTES.md.
      {"yolo_msgs/DetectionArray",
       {.object_type = ObjectType::kImageAnnotations,
        .parse_scalars = &RosParser::parseYoloScalars,
        .parse_object = &RosParser::parseYoloDetectionArray}},
      // A pose array becomes a canonical PosesInFrame object (poses only —
      // rendering style is viewer-side). Pose arrays can be huge (an AMCL
      // particle cloud carries 1000s of poses), so the scalar route applies the
      // large-array discard policy: small arrays stay plottable, big ones don't
      // explode into thousands of columns.
      {"geometry_msgs/PoseArray",
       {.object_type = ObjectType::kPosesInFrame,
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
        .parse_object = &RosParser::parsePoseArray}},
      // Foxglove's own ROS message: a bare Time + frame_id + Pose[]. Same
      // canonical target and large-array scalar policy as PoseArray.
      {"foxglove_msgs/PosesInFrame",
       {.object_type = ObjectType::kPosesInFrame,
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
        .parse_object = &RosParser::parseFoxglovePosesInFrame}},
      // A path is a trajectory of stamped poses; emit them as one PosesInFrame
      // in the path's frame (per-pose stamps dropped — see parsePath).
      {"nav_msgs/Path",
       {.object_type = ObjectType::kPosesInFrame,
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
        .parse_object = &RosParser::parsePath}},

      // ----- Specialized scalar schemas -----
      // wrapVoidHandler<Handler> is a member-fn-template; its address is a
      // member-fn-ptr matching parse_scalars, so it slots in directly.
      {"std_msgs/Empty", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleEmpty>}},
      {"geometry_msgs/Pose", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handlePose>}},
      // Dual route: a single stamped pose flattens to per-axis scalars AND
      // emits a one-element PosesInFrame object (like TransformStamped does for
      // FrameTransforms).
      {"geometry_msgs/PoseStamped",
       {.object_type = ObjectType::kPosesInFrame,
        .parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handlePoseStamped>,
        .parse_object = &RosParser::parsePoseStampedObject}},
      {"geometry_msgs/Transform", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleTransform>}},
      {"geometry_msgs/TransformStamped",
       {.object_type = ObjectType::kFrameTransforms,
        .parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleTransformStamped>,
        .parse_object = &RosParser::parseTransformStampedObject}},
      {"sensor_msgs/Imu", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleImu>}},
      // Dual route: the pose (of child_frame_id, in the header frame) emits a
      // one-element PosesInFrame for the 3D view, alongside the per-axis scalar
      // flatten (handleOdometry) of pose + twist + covariances.
      {"nav_msgs/Odometry",
       {.object_type = ObjectType::kPosesInFrame,
        .parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleOdometry>,
        .parse_object = &RosParser::parseOdometryObject}},
      {"sensor_msgs/JointState", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleJointState>}},
      {"diagnostic_msgs/DiagnosticArray",
       {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleDiagnosticArray>}},
      {"data_tamer_msgs/Schemas", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleDataTamerSchemas>}},
      {"data_tamer_msgs/Snapshot", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleDataTamerSnapshot>}},
      {"pal_statistics_msgs/StatisticsNames",
       {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handlePalStatisticsNames>}},
      {"pal_statistics_msgs/StatisticsValues",
       {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handlePalStatisticsValues>}},
      {"plotjuggler_msgs/StatisticsNames",
       {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handlePalStatisticsNames>}},
      {"plotjuggler_msgs/StatisticsValues",
       {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handlePalStatisticsValues>}},
      {"tsl_msgs/TSLDefinition", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleTSLDefinition>}},
      {"tsl_msgs/TSLValues", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleTSLValues>}},

      // ----- Default entry -----
      // Used by bindSchema for any ROS schema not matched above.
      // Drives the generic rosx_introspection walker that flattens
      // nested messages into one column per primitive field.
      {CatalogEntry::kDefault, {.parse_scalars = &RosParser::parseScalarsGeneric}},
  };
  return kMap;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

PJ::Status RosParser::bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) {
  // The schema arrives as raw bytes; rosx_introspection consumes it as a
  // std::string (the textual .msg or IDL definition).
  type_name_ = std::string(type_name);
  schema_definition_.assign(reinterpret_cast<const char*>(schema.data()), schema.size());
  schema_bound_ = true;
  schema_compiled_ = false;
  parser_.reset();
  has_header_ = false;
  quaternions_.clear();

  // Let the SDK base class record the bound type and run its own bind
  // bookkeeping (host registration, dialog config, …). We hand it the
  // ORIGINAL type_name (with the "/msg/" segment if present), so that
  // bound_type_name_ matches what the host will pass to classifySchema /
  // parseScalars / parseObject later. The internal `msg_type` is used only
  // for catalog lookup below.
  if (auto status = PJ::MessageParserPluginBase::bindSchema(type_name_, schema); !status) {
    return status;
  }

  // Generic scalar handler first, so a schema that fails to compile below
  // still binds; the compile error then resurfaces through loadConfig() or the
  // first parse, which recompile — exactly the pre-existing failure surface.
  registerBoundSchemaHandler(catalog().at(CatalogEntry::kDefault));

  if (schema_format_configured_) {
    return compileBoundSchema(true);
  }

  // No config yet — but classify_schema must already be correct right after
  // bind_schema (see message_parser_protocol.h; the demand-subscription host
  // classifies advertised topics through a throwaway bind→classify handle with
  // no parser_config_json). The schema encoding only travels in loadConfig(),
  // so infer the format from the inputs' shape — OMG IDL uses scoped
  // "pkg::Type" names and `module` blocks — and compile eagerly, best-effort:
  // try the inferred format, then the other. If both fail, stay bound on the
  // generic handler. A later loadConfig() with a different explicit encoding
  // recompiles via its format_changed path, so a mis-inference self-corrects
  // before any payload is parsed.
  const bool looks_idl =
      type_name_.find("::") != std::string::npos || schema_definition_.find("module ") != std::string::npos;
  const RosMsgParser::SchemaFormat first = looks_idl ? RosMsgParser::DDS_IDL : RosMsgParser::ROS_MSG;
  const RosMsgParser::SchemaFormat second = looks_idl ? RosMsgParser::ROS_MSG : RosMsgParser::DDS_IDL;
  for (const RosMsgParser::SchemaFormat format : {first, second}) {
    schema_format_ = format;
    schema_encoding_ = (format == RosMsgParser::DDS_IDL) ? "omgidl" : "ros2msg";
    if (compileBoundSchema(true)) {
      return PJ::okStatus();
    }
  }
  schema_format_ = RosMsgParser::ROS_MSG;
  schema_encoding_ = "ros2msg";
  return PJ::okStatus();
}

void RosParser::registerBoundSchemaHandler(const CatalogEntry& entry) {
  // Bind the catalog entry's member-function pointers to `this` and
  // register a single SchemaHandler with the host. The per-instance
  // handler table ends up with exactly one entry for this bound schema.
  //
  // The catalog still stores handlers that return the bare field vector /
  // BuiltinObject (the pre-0.3 shape). The SchemaHandler callables now return
  // ScalarRecord / ObjectRecord, which wrap that payload with an optional
  // parser-controlled timestamp. We bridge the two here: run the handler,
  // then wrap its result. When use_embedded_timestamp_ is on we carry
  // current_timestamp_ as the record's ts — the handlers resolve that member
  // to the payload-embedded Header stamp (readHeader / flattenGeneric), so
  // exposing it through the record makes the host honor it on the
  // pure-functional parseScalars / parseObject path, not just the legacy
  // parse() route. When the option is off ts stays nullopt and the host uses
  // the message receive time, exactly as before.
  PJ::sdk::SchemaHandler handler;
  handler.object_type = entry.object_type;
  if (entry.parse_scalars) {
    handler.parse_scalars = [this, fn = entry.parse_scalars](
                                PJ::Timestamp ts,
                                PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
      if (auto status = (this->*fn)(ts, payload); !status) {
        return PJ::unexpected(std::move(status).error());
      }
      PJ::sdk::ScalarRecord record;
      if (use_embedded_timestamp_) {
        record.ts = current_timestamp_;
      }
      record.fields.assign(fields_.begin(), fields_.end());
      return record;
    };
  }
  if (entry.parse_object) {
    // Object handlers already return a fully-formed ObjectRecord (the embedded
    // Header stamp is exposed via ObjectRecord::ts inside each handler), so the
    // catalog entry binds straight onto the SchemaHandler with no adapter.
    handler.parse_object = std::bind_front(entry.parse_object, this);
  }
  // Register under the original type_name (matches the key the host uses
  // when calling classifySchema / parseScalars / parseObject — keeps lookups
  // symmetric regardless of "/msg/" presence).
  registerSchemaHandler(type_name_, std::move(handler));
  // Only once the handler is registered, so parse() never runs a route the
  // base class does not have.
  scalar_route_ = entry.parse_scalars;
}

RosParser::CatalogEntry RosParser::selectCatalogEntry(const std::string& msg_type) const {
  // Catalog lookup: exact match for this schema, otherwise the kDefault
  // entry (generic introspection fallback). kDefault is guaranteed to be
  // present in the catalog, so the second find always hits.
  const std::string catalog_type = normalizedMessageType(msg_type, RosMsgParser::ROS_MSG);
  auto it = catalog().find(catalog_type);
  if (it == catalog().end()) {
    it = catalog().find(CatalogEntry::kDefault);
  }
  if (catalog_type == "diagnostic_msgs/DiagnosticArray" && !diag_layout_known_) {
    it = catalog().find(CatalogEntry::kDefault);  // a status layout the specialized reader cannot follow
  }
  CatalogEntry entry = it->second;

  // Topic-conditional override: a std_msgs/String on a robot_description topic
  // carries a URDF/SDF/MJCF model, not a generic string. The catalog keys on
  // type name, which can't distinguish this — so dispatch here by topic name.
  // Matches the bare topic and any namespace-prefixed "<ns>/robot_description"
  // (e.g. "/my_robot/robot_description").
  const bool robot_description_topic =
      topic_name_ == "robot_description" || topic_name_.ends_with("/robot_description");
  if (catalog_type == "std_msgs/String" && robot_description_topic) {
    // Object-only: the URDF/SDF/MJCF text is consumed as a model, not stored
    // as a giant string column in the datastore. The empty scalar route is
    // still required, as for markers: without parse_scalars the host's
    // eager-scalar ingest aborts the push and the model never reaches the store.
    entry = CatalogEntry{
        .object_type = PJ::sdk::BuiltinObjectType::kRobotDescription,
        .parse_scalars = &RosParser::parseScalarsObjectOnly,
        .parse_object = &RosParser::parseRobotDescription};
  }
  return entry;
}

PJ::Status RosParser::compileBoundSchema(bool register_specialized_handler) {
  if (!schema_bound_) {
    return PJ::unexpected(std::string("no schema bound"));
  }
  if (schema_compiled_) {
    // The per-message path (parseScalarsGeneric) lands here: no work, no allocation.
    if (register_specialized_handler) {
      registerBoundSchemaHandler(selectCatalogEntry(normalizedMessageType(type_name_, schema_format_)));
    }
    return PJ::okStatus();
  }
  const std::string msg_type = normalizedMessageType(type_name_, schema_format_);

  // Normalize root names to the conventions used by rosx_introspection. ROS 2
  // .msg schemas use "pkg/msg/Type" externally and "pkg/Type" internally;
  // IDL schemas retain module scopes internally, such as "pkg::msg/Type".
  // Compile the message definition once and keep the rosx_introspection
  // parser cached on this instance — it is reused for every message of
  // this type. The array policy controls how variable-length fields are
  // truncated by the generic introspection walker.
  try {
    const auto definition = schema_format_ == RosMsgParser::DDS_IDL && schema_definition_.starts_with("====")
                                ? ros2IdlDefinition(type_name_, schema_definition_)
                                : schema_definition_;
    parser_.emplace("", RosMsgParser::ROSType(msg_type), definition, schema_format_);
    leaf_names_.clear();  // keyed on the previous parser's schema nodes
    auto policy =
        discard_large_arrays_ ? RosMsgParser::Parser::DISCARD_LARGE_ARRAYS : RosMsgParser::Parser::KEEP_LARGE_ARRAYS;
    parser_->setMaxArrayPolicy(policy, max_array_size_);
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("failed to parse ROS schema: ") + e.what());
  }

  // Cache schema-derived flags (has_header_, quaternion prefixes, …) and
  // prepare the wire-format deserializer (ROS 1 binary vs ROS 2 CDR).
  detectSchemaFeatures();
  ensureDeserializer();

  // Humble+ Marker adds texture and mesh fields. Inspect the parsed Marker
  // fields so comments or unrelated bundled definitions cannot change its layout.
  const std::string ros_type = normalizedMessageType(msg_type, RosMsgParser::ROS_MSG);
  if (ros_type == "visualization_msgs/Marker" || ros_type == "visualization_msgs/MarkerArray") {
    marker_has_texture_block_ = false;
    marker_has_mesh_file_ = false;
    for (const auto& [type, message] : parser_->getSchema()->msg_library) {
      if (normalizedMessageType(type.baseName(), RosMsgParser::ROS_MSG) != "visualization_msgs/Marker") {
        continue;
      }
      for (const auto& field : message->fields()) {
        marker_has_texture_block_ |= field.name() == "uv_coordinates";
        marker_has_mesh_file_ |= field.name() == "mesh_file";
      }
      break;
    }
  }
  if (ros_type == "diagnostic_msgs/DiagnosticArray") {
    diag_layout_known_ = readDiagnosticLayout();
  }
  if (register_specialized_handler) {
    registerBoundSchemaHandler(selectCatalogEntry(msg_type));
  }

  // Flip the flag only after the handler is registered: if registration throws
  // (e.g. bad_alloc), schema_compiled_ stays false so the next compile — the
  // eager-inference loop's second format, or a later loadConfig()/parse —
  // recompiles cleanly instead of early-returning on a half-compiled schema.
  schema_compiled_ = true;

  return PJ::okStatus();
}

bool RosParser::readDiagnosticLayout() {
  const RosMsgParser::ROSMessage* status = nullptr;
  const RosMsgParser::ROSMessage* key_value = nullptr;
  for (const auto& [type, message] : parser_->getSchema()->msg_library) {
    const std::string name = normalizedMessageType(type.baseName(), RosMsgParser::ROS_MSG);
    if (name == "diagnostic_msgs/DiagnosticStatus") {
      status = message.get();
    } else if (name == "diagnostic_msgs/KeyValue") {
      key_value = message.get();
    }
  }
  if (status == nullptr || key_value == nullptr) {
    return false;
  }
  // The serialized fields: constants (OK=0, WARN=1, ...) are not on the wire.
  const auto data_fields = [](const RosMsgParser::ROSMessage& message) {
    std::vector<const RosMsgParser::ROSField*> fields;
    for (const auto& field : message.fields()) {
      if (!field.isConstant()) {
        fields.push_back(&field);
      }
    }
    return fields;
  };
  const auto is_string = [](const RosMsgParser::ROSField* field) {
    return !field->isArray() && field->type().typeID() == RosMsgParser::STRING;
  };
  const auto kv = data_fields(*key_value);
  if (kv.size() != 2 || kv[0]->name() != "key" || kv[1]->name() != "value" || !is_string(kv[0]) || !is_string(kv[1])) {
    return false;
  }
  const auto fields = data_fields(*status);
  if (fields.size() != 5 || fields[4]->name() != "values" || !fields[4]->isArray()) {
    return false;
  }
  std::array<DiagField, 4> order{};
  std::array<bool, 4> seen{};
  for (size_t i = 0; i < order.size(); ++i) {
    const auto* field = fields[i];
    const auto id = field->type().typeID();
    if (field->name() == "level" && !field->isArray() &&
        (id == RosMsgParser::BYTE || id == RosMsgParser::UINT8 || id == RosMsgParser::INT8 ||
         id == RosMsgParser::CHAR)) {
      order[i] = DiagField::kLevel;
    } else if (field->name() == "name" && is_string(field)) {
      order[i] = DiagField::kName;
    } else if (field->name() == "message" && is_string(field)) {
      order[i] = DiagField::kMessage;
    } else if (field->name() == "hardware_id" && is_string(field)) {
      order[i] = DiagField::kHardwareId;
    } else {
      return false;
    }
    auto& was_seen = seen[static_cast<size_t>(order[i])];
    if (was_seen) {
      return false;
    }
    was_seen = true;
  }
  diag_order_ = order;
  return true;
}

std::string RosParser::saveConfig() const {
  nlohmann::json cfg;
  PJ::sdk::arrayLimitToJson(cfg, static_cast<uint32_t>(max_array_size_), !discard_large_arrays_);
  cfg["use_embedded_timestamp"] = use_embedded_timestamp_;
  cfg["boolean_strings_to_number"] = boolean_strings_to_number_;
  cfg["remove_suffix_from_strings"] = remove_suffix_from_strings_;
  cfg["serialization"] = use_ros1_ ? "ros1" : "cdr";
  cfg["schema_encoding"] = schema_encoding_;
  if (!topic_name_.empty()) {
    cfg["topic_name"] = topic_name_;
  }
  return cfg.dump();
}

PJ::Status RosParser::loadConfig(std::string_view config_json) {
  auto cfg = nlohmann::json::parse(config_json, nullptr, false);
  if (cfg.is_discarded()) {
    return PJ::okStatus();
  }

  const auto array_limit = PJ::sdk::arrayLimitFromJson(cfg);
  max_array_size_ = array_limit.max_size;
  discard_large_arrays_ = (array_limit.policy == PJ::sdk::ArrayPolicy::kSkip);
  use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
  boolean_strings_to_number_ = cfg.value("boolean_strings_to_number", false);
  remove_suffix_from_strings_ = cfg.value("remove_suffix_from_strings", false);
  topic_name_ = cfg.value("topic_name", std::string{});
  pal_statistics_key_ = palStatisticsKey(topic_name_);

  const std::string requested_schema_encoding = cfg.value("schema_encoding", cfg.value("encoding", std::string{}));
  if (!requested_schema_encoding.empty()) {
    auto schema_format = schemaEncodingToFormat(requested_schema_encoding);
    if (!schema_format) {
      return PJ::unexpected(std::move(schema_format).error());
    }
    const bool format_changed = schema_format->second != schema_format_;
    schema_encoding_ = std::move(schema_format->first);
    schema_format_ = schema_format->second;
    schema_format_configured_ = true;
    if (format_changed) {
      schema_compiled_ = false;
      parser_.reset();
      has_header_ = false;
      quaternions_.clear();
    }
  }

  bool new_ros1 = (cfg.value("serialization", "cdr") == "ros1");
  if (new_ros1 != use_ros1_) {
    use_ros1_ = new_ros1;
    deserializer_.reset();  // force re-creation
  }

  if (parser_.has_value()) {
    auto policy =
        discard_large_arrays_ ? RosMsgParser::Parser::DISCARD_LARGE_ARRAYS : RosMsgParser::Parser::KEEP_LARGE_ARRAYS;
    parser_->setMaxArrayPolicy(policy, max_array_size_);
  }
  ensureDeserializer();
  if (schema_bound_) {
    if (auto status = compileBoundSchema(true); !status) {
      return status;
    }
  }
  return PJ::okStatus();
}

PJ::Status RosParser::parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) {
  if (!writeHostBound()) {
    return PJ::unexpected(std::string("write host not bound"));
  }
  if (scalar_route_ == nullptr) {
    // No schema bound, so no handler registered: the base reports that.
    auto record = parseScalars(timestamp_ns, payload);
    return record ? PJ::okStatus() : PJ::unexpected(std::move(record).error());
  }
  // The same route parseScalars dispatches to, called directly: fields_ is
  // appended as it is, with no per-message copy of every field name.
  if (auto status = (this->*scalar_route_)(timestamp_ns, payload); !status) {
    return status;
  }
  if (fields_.empty()) {
    return PJ::okStatus();
  }
  // Honor the parser-controlled timestamp (use_embedded_timestamp_, resolved by
  // the handler into current_timestamp_); otherwise the message receive time.
  const PJ::Timestamp ts = use_embedded_timestamp_ ? current_timestamp_ : timestamp_ns;
  return bound_field_cache_.append(writeHost(), ts, fields_.span(), fields_.nameIds());
}

// ---------------------------------------------------------------------------
// Generic scalar route. Walks any ROS message whose schema rosx_introspection
// understands, honoring the user-configured array policy. Used as the
// default-handler scalar route; also reused as the building block for
// parseScalarsDiscardingLargeArrays below.
// ---------------------------------------------------------------------------

PJ::Status RosParser::parseScalarsGeneric(PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  if (auto status = compileBoundSchema(false); !status) {
    return PJ::unexpected(std::move(status).error());
  }
  if (!parser_.has_value()) {
    return PJ::unexpected(std::string("no schema bound"));
  }
  ensureDeserializer();
  fields_.clear();
  current_timestamp_ = ts;
  flattenGeneric(payload);
  return PJ::okStatus();
}

// ---------------------------------------------------------------------------
// Scalar route for builtin-object schemas. Delegates to parseScalarsGeneric
// after flipping the parser to DISCARD_LARGE_ARRAYS so the bulk byte payload
// (Image::data, PointCloud2::data, …) is dropped automatically while small
// metadata (height, width, encoding, fields[].name, …) survives as scalars.
// The user-configured array policy is restored on exit.
// ---------------------------------------------------------------------------

PJ::Status RosParser::parseScalarsDiscardingLargeArrays(PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  if (auto status = compileBoundSchema(false); !status) {
    return PJ::unexpected(std::move(status).error());
  }
  if (!parser_.has_value()) {
    return PJ::unexpected(std::string("no schema bound"));
  }
  parser_->setMaxArrayPolicy(RosMsgParser::Parser::DISCARD_LARGE_ARRAYS, max_array_size_);
  auto result = parseScalarsGeneric(ts, payload);
  auto restored =
      discard_large_arrays_ ? RosMsgParser::Parser::DISCARD_LARGE_ARRAYS : RosMsgParser::Parser::KEEP_LARGE_ARRAYS;
  parser_->setMaxArrayPolicy(restored, max_array_size_);
  return result;
}

// ---------------------------------------------------------------------------
// Slim scalar route for object-only schemas (markers): no meaningful scalar
// columns, but a non-null parse_scalars is required so the host's eager-scalar
// ingest path (RosParser::parse -> parseScalars) succeeds and the canonical
// object reaches the ObjectStore under ANY ObjectIngestPolicy, not only
// kPureLazy. Returns an empty field set (parse() short-circuits to ok on empty
// fields, so no scalar row is written). Decodes nothing — zero cost, zero risk.
// ---------------------------------------------------------------------------

PJ::Status RosParser::parseScalarsObjectOnly(PJ::Timestamp ts, PJ::Span<const uint8_t> /*payload*/) {
  current_timestamp_ = ts;
  fields_.clear();
  return PJ::okStatus();
}

// ---------------------------------------------------------------------------
// Shared plumbing for the slim direct-CDR scalar routes (the hand-written
// header/metadata walkers that back the object-bearing schemas). Same setup and
// harvest halves as wrapVoidHandler, minus the introspection parser: these
// routes never need a compiled schema, only the deserializer.
// ---------------------------------------------------------------------------

void RosParser::beginDirectScalarRead(PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  ensureDeserializer();
  fields_.clear();
  current_timestamp_ = ts;
  deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.data(), payload.size()));
}

// ---------------------------------------------------------------------------
// Header-only scalar route, for object-bearing schemas whose payload has nothing
// plottable of its own but does lead with a std_msgs/Header:
//
//   visualization_msgs/Marker                       — pure 3D scene content.
//   point_cloud_interfaces/CompressedPointCloud2    — the cloud is an opaque
//       compressed blob, so no point count can be measured from it: the
//       compressed byte size divided by point_step is meaningless, and
//       width * height would be the producer's claim rather than a measurement
//       (the uncompressed PointCloud2 route emits a real count instead).
//
// The route also has to exist at all: an object-only entry (no parse_scalars)
// makes the host's eager-scalar ingest abort the push on any non-kPureLazy
// policy, and the canonical object never reaches the ObjectStore.
// ---------------------------------------------------------------------------

PJ::Status RosParser::parseHeaderOnlyScalars(PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  try {
    beginDirectScalarRead(ts, payload);
    emitHeader(readHeader());
    return PJ::okStatus();
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("header scalars: CDR read error: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------------------

void RosParser::ensureDeserializer() {
  bool need_create = !deserializer_ || (use_ros1_ == deserializer_->isROS2());
  if (need_create) {
    if (use_ros1_) {
      deserializer_ = std::make_unique<RosMsgParser::ROS_Deserializer>();
    } else {
      deserializer_ = std::make_unique<RosMsgParser::NanoCDR_Deserializer>();
    }
  }
}

RosMsgParser::Span<const uint8_t> RosParser::readByteSequence() {
  // End of the payload, captured before the read: the check is on the span's
  // end pointer, so the CDR alignment padding consumed before the length word
  // cannot give the declared length any slack.
  const uint8_t* end = deserializer_->getCurrentPtr() + deserializer_->bytesLeft();
  const auto span = deserializer_->deserializeByteSequence();
  if (!span.empty() && span.data() + span.size() > end) {
    // The declared length ran past the payload. deserializeByteSequence has already
    // advanced the cursor past the end, so both this span AND any field the handler
    // reads next would be out of bounds; abort now (caught upstream -> Expected error).
    throw std::runtime_error("CDR byte sequence length exceeds message payload (truncated or corrupt message)");
  }
  return span;
}

void RosParser::detectSchemaFeatures() {
  const auto& schema = parser_->getSchema();
  const auto& root_fields = schema->root_msg->fields();

  has_header_ =
      !root_fields.empty() &&
      normalizedMessageType(root_fields.front().type().baseName(), RosMsgParser::ROS_MSG) == "std_msgs/Header";

  quaternions_.clear();
  findQuaternionPrefixes(schema->root_msg.get(), "", schema->msg_library);
}

void RosParser::findQuaternionPrefixes(
    const RosMsgParser::ROSMessage* msg, const std::string& prefix, const RosMsgParser::RosMessageLibrary& lib) {
  for (const auto& field : msg->fields()) {
    if (field.isConstant()) {
      continue;
    }

    std::string fp = prefix + "/" + field.name();
    const auto& type = field.type();

    if (normalizedMessageType(type.baseName(), RosMsgParser::ROS_MSG) == "geometry_msgs/Quaternion") {
      // For arrays, the flattened name includes [i]; skip at bind time.
      if (!field.isArray()) {
        static constexpr std::array<std::string_view, 4> kComponents = {"/x", "/y", "/z", "/w"};
        static constexpr std::array<std::string_view, 3> kAngles = {"/roll", "/pitch", "/yaw"};
        auto& q = quaternions_.emplace_back();
        for (size_t k = 0; k < kComponents.size(); ++k) {
          q.component[k] = fp;
          q.component[k] += kComponents[k];
          q.index[k] = std::numeric_limits<size_t>::max();  // found by name on first use
        }
        for (size_t k = 0; k < kAngles.size(); ++k) {
          q.angle[k] = fp;
          q.angle[k] += kAngles[k];
          q.angle_id[k] = fields_.newNameId();
        }
      }
    } else if (type.typeID() == RosMsgParser::OTHER) {
      auto it = lib.find(type);
      if (it != lib.end() && !field.isArray()) {
        findQuaternionPrefixes(it->second.get(), fp, lib);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Field accumulation helpers
// ---------------------------------------------------------------------------

void RosParser::addField(std::string_view name, double value) {
  fields_.add(name, PJ::sdk::ValueRef{value});
}

void RosParser::addField(std::string_view name, PJ::sdk::ValueRef value) {
  fields_.add(name, value);
}

void RosParser::addStringField(std::string_view name, std::string_view value) {
  fields_.addString(name, value);
}

// ---------------------------------------------------------------------------
// Emit record
// ---------------------------------------------------------------------------

PJ::Status RosParser::emitRecord(PJ::Timestamp ts) {
  return bound_field_cache_.append(writeHost(), ts, fields_.span(), fields_.nameIds());
}

// ---------------------------------------------------------------------------
// Header helpers
// ---------------------------------------------------------------------------

const RosParser::HeaderData& RosParser::readHeader() {
  // Read into the member, so frame_id keeps its capacity from message to message.
  HeaderData& h = header_;
  h.seq = 0;
  // The stamp's `sec` word has a DIFFERENT SIGNEDNESS on the two wires, and the
  // raw 4 bytes are identical either way — only the interpretation differs:
  //   ROS 1  std_msgs/Header { uint32 seq; time stamp; ... }
  //          ros::Time.sec is UINT32.
  //   ROS 2  std_msgs/Header { builtin_interfaces/Time stamp; ... }  (no seq)
  //          builtin_interfaces/Time.sec is INT32.
  // Reading the ROS 2 word as unsigned turned any negative stamp into ~4.29e9
  // (a 2106-ish date), which is not a rounding artifact but a wholly wrong
  // value: negative stamps are legal and do occur (pre-epoch or relative clocks).
  if (!deserializer_->isROS2()) {
    h.seq = deserializer_->deserializeUInt32();
    h.sec = static_cast<int64_t>(deserializer_->deserializeUInt32());
  } else {
    h.sec = static_cast<int64_t>(static_cast<int32_t>(deserializer_->deserializeUInt32()));
  }
  h.nsec = deserializer_->deserializeUInt32();

  if (use_embedded_timestamp_) {
    const int64_t ts_ns = h.sec * 1000000000LL + static_cast<int64_t>(h.nsec);
    // A non-positive stamp means "unset" (or a pre-epoch clock we cannot file
    // rows under), so the embedded clock is not adopted — unchanged behavior,
    // and it is what keeps a now-correctly-negative sec from being adopted.
    if (ts_ns > 0) {
      current_timestamp_ = ts_ns;
    }
  }

  deserializer_->deserializeString(h.frame_id);
  return h;
}

int64_t RosParser::readBareTime() {
  // The foxglove_msgs schemas lead with a bare stamp whose `sec` word has the
  // same wire-dependent signedness as the Header stamp: ROS 2
  // builtin_interfaces/Time.sec is int32 (negative stamps are legal), ROS 1
  // `time`.sec is uint32 (its upper half would go negative if read signed).
  const uint32_t raw_sec = deserializer_->deserializeUInt32();
  const int64_t sec =
      deserializer_->isROS2() ? static_cast<int64_t>(static_cast<int32_t>(raw_sec)) : static_cast<int64_t>(raw_sec);
  const uint32_t nsec = deserializer_->deserializeUInt32();
  const int64_t ts_ns = sec * 1000000000LL + static_cast<int64_t>(nsec);
  if (use_embedded_timestamp_ && ts_ns > 0) {
    current_timestamp_ = ts_ns;
  }
  return ts_ns;
}

void RosParser::emitHeader(const HeaderData& h) {
  double stamp = static_cast<double>(h.sec) + static_cast<double>(h.nsec) * 1e-9;
  addField("/header/stamp", stamp);
  addStringField("/header/frame_id", h.frame_id);
  if (!deserializer_->isROS2()) {
    addField("/header/seq", static_cast<double>(h.seq));
  }
}

// ---------------------------------------------------------------------------
// Generic path
// ---------------------------------------------------------------------------

namespace {

bool sameLeaf(const RosMsgParser::FieldLeaf& a, const RosMsgParser::FieldLeaf& b) {
  if (a.node != b.node || !(a.index_array == b.index_array) || a.key_suffixes.size() != b.key_suffixes.size()) {
    return false;
  }
  for (size_t i = 0; i < a.key_suffixes.size(); ++i) {
    const auto& x = a.key_suffixes[i];
    const auto& y = b.key_suffixes[i];
    if (x.len != y.len || std::memcmp(x.data, y.data, x.len) != 0) {
      return false;
    }
  }
  return true;
}

// Streams the rosx schema walk straight into RosParser::fields_, in walk order,
// with no FlatMessage in between (which copied every leaf and every string).
// Keeps the first two values: with a std_msgs/Header they are its stamp.
class FlattenWriter final : public RosMsgParser::MessageWriter {
 public:
  explicit FlattenWriter(RosParser& parser) : parser_(parser) {}

  void writeValue(const RosMsgParser::FieldLeaf& leaf, const RosMsgParser::Variant& value) override {
    if (count_ < first_.size()) {
      first_[count_] = value;
    }
    const auto& name = parser_.leafName(count_++, leaf);
    parser_.fields_.add(name.name, variantToValueRef(value), name.name_id);
  }

  void writeString(const RosMsgParser::FieldLeaf& leaf, const std::string& text) override {
    const auto& name = parser_.leafName(count_++, leaf);
    double numeric = 0.0;
    // Apply the string-to-number toggles before falling back to the string
    // representation. parseStringAsDouble returns false when either both
    // toggles are off or no rule matches; in that case we keep the string
    // verbatim, copied into fields_ (ValueRef holds a non-owning string_view).
    if ((parser_.boolean_strings_to_number_ || parser_.remove_suffix_from_strings_) &&
        parseStringAsDouble(text, numeric, parser_.remove_suffix_from_strings_, parser_.boolean_strings_to_number_)) {
      parser_.fields_.add(name.name, PJ::sdk::ValueRef{numeric}, name.name_id);
    } else {
      parser_.fields_.addString(name.name, text, name.name_id);
    }
  }

  void writeEnum(const RosMsgParser::FieldLeaf& leaf, int32_t value, const std::string& /*name*/) override {
    writeValue(leaf, RosMsgParser::Variant(value));  // as rosx's FlatMessageWriter does
  }

  [[nodiscard]] size_t count() const {
    return count_;
  }
  [[nodiscard]] double first(size_t i) const {
    return first_[i].convert<double>();
  }

 private:
  RosParser& parser_;
  size_t count_ = 0;
  std::array<RosMsgParser::Variant, 2> first_{};
};

}  // namespace

const RosParser::CachedLeaf& RosParser::leafName(size_t index, const RosMsgParser::FieldLeaf& leaf) {
  if (index >= leaf_names_.size()) {
    leaf_names_.resize(index + 1);
  }
  auto& cached = leaf_names_[index];
  if (!sameLeaf(cached.leaf, leaf)) {
    // The entry matches nothing until it is complete: a throw while rendering
    // must not leave the old name filed under the new leaf (or the reverse).
    cached.leaf.node = nullptr;
    leaf.toStr(cached.name);
    cached.name_id = fields_.newNameId();
    cached.leaf.index_array = leaf.index_array;
    cached.leaf.key_suffixes = leaf.key_suffixes;
    cached.leaf.node = leaf.node;
  }
  return cached;
}

void RosParser::flattenGeneric(PJ::Span<const uint8_t> payload) {
  // Combined /header/stamp (seconds) for ROS 2, whose std_msgs/Header flattens
  // to split sec/nanosec leaves: the single plottable stamp series the
  // specialized handlers emit via emitHeader(), a diagnostic aid for clock
  // offset/skew against the message (log/publish) time. Independent of the
  // use_embedded_timestamp toggle. ROS 1 must NOT get it: its `time` builtin
  // already flattens to one /header/stamp leaf in seconds, and a second column
  // of the same name makes the datastore reject the whole record.
  // It comes first in the record; its slot is filled after the walk.
  const bool combined_stamp = has_header_ && deserializer_->isROS2();
  const size_t stamp_slot = fields_.size();
  if (combined_stamp) {
    addField("/header/stamp", 0.0);
  }

  FlattenWriter writer(*this);
  try {
    parser_->walkSchema(
        RosMsgParser::Span<const uint8_t>(payload.data(), payload.size()), deserializer_.get(), &writer);
  } catch (const std::bad_alloc&) {
    // Out of memory is not a malformed message: let it reach the caller (the SDK
    // trampoline turns it into an error) instead of reporting "no record".
    throw;
  } catch (const std::exception&) {
    // CDR deserialization failed; bail and let the empty fields_
    // signal "no record" to the outer parse() caller. The SDK base
    // class surfaces parse errors via PJ::unexpected() — best-effort
    // mid-flatten errors are silently dropped.
    fields_.clear();
    return;
  }

  // Embedded timestamp: the first two values are the header's stamp.
  if (use_embedded_timestamp_ && has_header_ && writer.count() >= 2) {
    double ts = 0;
    if (deserializer_->isROS2()) {
      ts = writer.first(0) + 1e-9 * writer.first(1);
    } else {
      // ROS1: value[1] is stamp (Time builtin)
      ts = writer.first(1);
    }
    if (ts > 0) {
      current_timestamp_ = static_cast<int64_t>(ts * 1e9);
    }
  }

  if (combined_stamp) {
    // has_header_: the walk writes the header's stamp/sec and stamp/nanosec
    // first, and a walk that throws returned above.
    assert(writer.count() >= 2);
    fields_.setValue(stamp_slot, PJ::sdk::ValueRef{writer.first(0) + 1e-9 * writer.first(1)});
  }

  addQuaternionRPY();
}

void RosParser::addQuaternionRPY() {
  // Only the walked fields are searched, not the angles added below.
  const size_t n = fields_.size();
  for (auto& q : quaternions_) {
    std::array<double, 4> v{};
    for (size_t k = 0; k < v.size(); ++k) {
      // The position from the previous message, checked by name; searched again
      // (first field of that name) only when the message shape moved it.
      size_t& i = q.index[k];
      if (i >= n || fields_[i].name != q.component[k]) {
        i = 0;
        while (i < n && fields_[i].name != q.component[k]) {
          ++i;
        }
      }
      v[k] = i < n ? valueRefAsDouble(fields_[i].value) : 0.0;
    }
    const auto rpy = quaternionToRPY(v[0], v[1], v[2], v[3]);
    fields_.add(q.angle[0], PJ::sdk::ValueRef{rpy.roll}, q.angle_id[0]);
    fields_.add(q.angle[1], PJ::sdk::ValueRef{rpy.pitch}, q.angle_id[1]);
    fields_.add(q.angle[2], PJ::sdk::ValueRef{rpy.yaw}, q.angle_id[2]);
  }
}

}  // namespace ros_parser_detail
