#include <cctype>
#include <cstdlib>
#include <functional>
#include <pj_array_policy/array_policy.hpp>
#include <string>

#include "ros_parser_internal.hpp"

namespace ros_parser_detail {

namespace {

// Port of the legacy PJ::ParseDouble helper (PJ3
// `plotjuggler-ros-plugins/src/parser_configuration.cpp:115-153`). Same
// three-stage semantics, std::strtod in place of boost::spirit:
//
//   1. Strict full-string double parse: success only when strtod consumes
//      every byte of `str`.
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

  char* parse_end = nullptr;
  double parsed = std::strtod(str.data(), &parse_end);
  if (parse_end == str.data() + str.size()) {
    value = parsed;
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
      const std::string prefix(str, 0, pos);
      parse_end = nullptr;
      parsed = std::strtod(prefix.data(), &parse_end);
      if (parse_end == prefix.data() + prefix.size()) {
        value = parsed;
        return true;
      }
    }
  }

  if (parse_boolean && str.size() >= 4 && str.size() <= 5) {
    std::string lower = str;
    std::transform(
        lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
    // OMG IDL schemas use scoped names such as "pkg::Type". rosx_introspection
    // expects the root type as "pkg/Type", matching PJ3's ParserOMGIDL.
    if (auto pos = msg_type.rfind("::"); pos != std::string::npos) {
      msg_type.replace(pos, 2, "/");
    }
  } else if (auto pos = msg_type.find("/msg/"); pos != std::string::npos) {
    msg_type.erase(pos, 4);
  }
  return msg_type;
}

PJ::Expected<std::pair<std::string, RosMsgParser::SchemaFormat>> schemaEncodingToFormat(std::string_view encoding) {
  if (encoding == "omgidl") {
    return std::make_pair(std::string("omgidl"), RosMsgParser::DDS_IDL);
  }
  if (encoding == "ros1msg" || encoding == "ros1") {
    return std::make_pair(std::string("ros1msg"), RosMsgParser::ROS_MSG);
  }
  if (encoding.empty() || encoding == "ros2msg" || encoding == "ros2") {
    return std::make_pair(std::string("ros2msg"), RosMsgParser::ROS_MSG);
  }
  return PJ::unexpected(std::string("unsupported ROS schema encoding: ") + std::string(encoding));
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
      {"sensor_msgs/PointCloud2",
       {.object_type = ObjectType::kPointCloud,
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
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
      // foxglove_msgs/CompressedPointCloud — opaque compressed cloud (draco/cloudini/…).
      // The parser repackages the blob + format; it does not decode. The scalar
      // route keeps frame_id / format plottable while discarding the data[] blob.
      {"foxglove_msgs/CompressedPointCloud",
       {.object_type = ObjectType::kCompressedPointCloud,
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
        .parse_object = &RosParser::parseFoxgloveCompressedPointCloud}},
      // point_cloud_interfaces/CompressedPointCloud2 — the point_cloud_transport
      // canonical compressed message. Same dual route as above.
      {"point_cloud_interfaces/CompressedPointCloud2",
       {.object_type = ObjectType::kCompressedPointCloud,
        .parse_scalars = &RosParser::parseScalarsDiscardingLargeArrays,
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
      // Markers are 3D scene content with no meaningful scalar columns, but they
      // still need a slim parse_scalars: an object-only entry makes the host's
      // eager-scalar ingest abort the push on any non-kPureLazy policy (e.g. live
      // streams, which set no override), silently dropping the object.
      // parseScalarsObjectOnly returns an empty row so the SceneEntities object
      // ingests under ANY policy. One SceneEntity per Marker (ADD/MODIFY) or a
      // SceneEntityDeletion (DELETE/DELETEALL) — see MARKER_NOTES.md.
      {"visualization_msgs/Marker",
       {.object_type = ObjectType::kSceneEntities,
        .parse_scalars = &RosParser::parseScalarsObjectOnly,
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

      // ----- Specialized scalar schemas -----
      // wrapVoidHandler<Handler> is a member-fn-template; its address is a
      // member-fn-ptr matching parse_scalars, so it slots in directly.
      {"std_msgs/Empty", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleEmpty>}},
      {"geometry_msgs/Pose", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handlePose>}},
      {"geometry_msgs/PoseStamped", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handlePoseStamped>}},
      {"geometry_msgs/Transform", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleTransform>}},
      {"geometry_msgs/TransformStamped",
       {.object_type = ObjectType::kFrameTransforms,
        .parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleTransformStamped>,
        .parse_object = &RosParser::parseTransformStampedObject}},
      {"sensor_msgs/Imu", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleImu>}},
      {"nav_msgs/Odometry", {.parse_scalars = &RosParser::wrapVoidHandler<&RosParser::handleOdometry>}},
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
  quaternion_prefixes_.clear();

  // Let the SDK base class record the bound type and run its own bind
  // bookkeeping (host registration, dialog config, …). We hand it the
  // ORIGINAL type_name (with the "/msg/" segment if present), so that
  // bound_type_name_ matches what the host will pass to classifySchema /
  // parseScalars / parseObject later. The internal `msg_type` is used only
  // for catalog lookup below.
  if (auto status = PJ::MessageParserPluginBase::bindSchema(type_name_, schema); !status) {
    return status;
  }

  // Runtime hosts in PJ4 call bindSchema() before loadConfig(). The selected
  // schema encoding therefore arrives later through parser_config_json. Keep a
  // generic scalar handler available immediately, then replace it with the
  // schema-specific handler once loadConfig() has selected ros2msg/omgidl.
  registerBoundSchemaHandler(catalog().at(CatalogEntry::kDefault));

  if (schema_format_configured_) {
    return compileBoundSchema(true);
  }
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
    auto fn = std::bind_front(entry.parse_scalars, this);
    handler.parse_scalars = [this, fn = std::move(fn)](
                                PJ::Timestamp ts,
                                PJ::Span<const uint8_t> payload) -> PJ::Expected<PJ::sdk::ScalarRecord> {
      auto fields = fn(ts, payload);
      if (!fields) {
        return PJ::unexpected(std::move(fields).error());
      }
      PJ::sdk::ScalarRecord record;
      if (use_embedded_timestamp_) {
        record.ts = current_timestamp_;
      }
      record.fields = std::move(*fields);
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
}

RosParser::CatalogEntry RosParser::selectCatalogEntry(const std::string& msg_type) const {
  // Catalog lookup: exact match for this schema, otherwise the kDefault
  // entry (generic introspection fallback). kDefault is guaranteed to be
  // present in the catalog, so the second find always hits.
  auto it = catalog().find(msg_type);
  if (it == catalog().end()) {
    it = catalog().find(CatalogEntry::kDefault);
  }
  CatalogEntry entry = it->second;

  // Topic-conditional override: a std_msgs/String on a robot_description topic
  // carries a URDF/SDF/MJCF model, not a generic string. The catalog keys on
  // type name, which can't distinguish this — so dispatch here by topic name.
  // Matches the bare topic and any namespace-prefixed "<ns>/robot_description"
  // (e.g. "/my_robot/robot_description").
  const bool robot_description_topic =
      topic_name_ == "robot_description" || topic_name_.ends_with("/robot_description");
  if (msg_type == "std_msgs/String" && robot_description_topic) {
    // Object-only: the URDF/SDF/MJCF text is consumed as a model, not stored
    // as a giant string column in the datastore.
    entry = CatalogEntry{
        .object_type = PJ::sdk::BuiltinObjectType::kRobotDescription,
        .parse_object = &RosParser::parseRobotDescription};
  }
  return entry;
}

PJ::Status RosParser::compileBoundSchema(bool register_specialized_handler) {
  if (!schema_bound_) {
    return PJ::unexpected(std::string("no schema bound"));
  }
  const std::string msg_type = normalizedMessageType(type_name_, schema_format_);
  if (schema_compiled_) {
    if (register_specialized_handler) {
      registerBoundSchemaHandler(selectCatalogEntry(msg_type));
    }
    return PJ::okStatus();
  }

  // Normalize root names to the conventions used by rosx_introspection. ROS 2
  // .msg schemas use "pkg/msg/Type" externally and "pkg/Type" internally;
  // OMG IDL schemas use scoped names externally and "pkg/Type" internally.
  // Compile the message definition once and keep the rosx_introspection
  // parser cached on this instance — it is reused for every message of
  // this type. The array policy controls how variable-length fields are
  // truncated by the generic introspection walker.
  try {
    parser_.emplace("", RosMsgParser::ROSType(msg_type), schema_definition_, schema_format_);
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

  // visualization_msgs/Marker has two wire layouts: ROS 2 humble+ added a
  // texture block (texture_resource / texture / uv_coordinates) and a
  // mesh_file field; EOL foxy/galactic and ROS 1 lack them. Sniff the bound
  // definition so the positional decoder consumes the correct variable tail.
  if (msg_type == "visualization_msgs/Marker" || msg_type == "visualization_msgs/MarkerArray") {
    marker_has_texture_block_ = schema_definition_.find("uv_coordinates") != std::string::npos;
    marker_has_mesh_file_ = schema_definition_.find("mesh_file") != std::string::npos;
  }
  schema_compiled_ = true;

  if (register_specialized_handler) {
    registerBoundSchemaHandler(selectCatalogEntry(msg_type));
  }

  return PJ::okStatus();
}

std::string RosParser::saveConfig() const {
  nlohmann::json cfg;
  pj::array_policy::arrayLimitToJson(cfg, static_cast<uint32_t>(max_array_size_), !discard_large_arrays_);
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

  const auto array_limit = pj::array_policy::arrayLimitFromJson(cfg);
  max_array_size_ = array_limit.max_size;
  discard_large_arrays_ = (array_limit.policy == pj::array_policy::ArrayPolicy::kSkip);
  use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
  boolean_strings_to_number_ = cfg.value("boolean_strings_to_number", false);
  remove_suffix_from_strings_ = cfg.value("remove_suffix_from_strings", false);
  topic_name_ = cfg.value("topic_name", std::string{});

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
      quaternion_prefixes_.clear();
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
  auto record = parseScalars(timestamp_ns, payload);
  if (!record) {
    return PJ::unexpected(std::move(record).error());
  }
  if (record->fields.empty()) {
    return PJ::okStatus();
  }
  // Honor the parser-controlled timestamp when the scalar handler set one
  // (use_embedded_timestamp_); otherwise fall back to the message receive
  // time. The handler already folded the embedded stamp into the record.
  const PJ::Timestamp ts = record->ts.value_or(timestamp_ns);
  return writeHost().appendRecord(
      ts, PJ::Span<const PJ::sdk::NamedFieldValue>(record->fields.data(), record->fields.size()));
}

// ---------------------------------------------------------------------------
// Generic scalar route. Walks any ROS message whose schema rosx_introspection
// understands, honoring the user-configured array policy. Used as the
// default-handler scalar route; also reused as the building block for
// parseScalarsDiscardingLargeArrays below.
// ---------------------------------------------------------------------------

PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> RosParser::parseScalarsGeneric(
    PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
  if (auto status = compileBoundSchema(false); !status) {
    return PJ::unexpected(std::move(status).error());
  }
  if (!parser_.has_value()) {
    return PJ::unexpected(std::string("no schema bound"));
  }
  ensureDeserializer();
  owned_fields_.clear();
  string_storage_.clear();
  named_fields_.clear();
  current_timestamp_ = ts;
  flattenGeneric(payload);

  std::vector<PJ::sdk::NamedFieldValue> out;
  out.reserve(owned_fields_.size());
  for (const auto& f : owned_fields_) {
    out.push_back({.name = f.name, .value = f.value});
  }
  return out;
}

// ---------------------------------------------------------------------------
// Scalar route for builtin-object schemas. Delegates to parseScalarsGeneric
// after flipping the parser to DISCARD_LARGE_ARRAYS so the bulk byte payload
// (Image::data, PointCloud2::data, …) is dropped automatically while small
// metadata (height, width, encoding, fields[].name, …) survives as scalars.
// The user-configured array policy is restored on exit.
// ---------------------------------------------------------------------------

PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> RosParser::parseScalarsDiscardingLargeArrays(
    PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
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

PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> RosParser::parseScalarsObjectOnly(
    PJ::Timestamp ts, PJ::Span<const uint8_t> /*payload*/) {
  current_timestamp_ = ts;
  return std::vector<PJ::sdk::NamedFieldValue>{};
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

void RosParser::detectSchemaFeatures() {
  const auto& schema = parser_->getSchema();
  const auto& root_fields = schema->root_msg->fields();

  has_header_ = !root_fields.empty() && root_fields.front().type().baseName() == "std_msgs/Header";

  quaternion_prefixes_.clear();
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

    if (type.baseName() == "geometry_msgs/Quaternion") {
      // For arrays, the flattened name includes [i]; skip at bind time.
      if (!field.isArray()) {
        quaternion_prefixes_.push_back(fp);
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

void RosParser::addField(const std::string& name, double value) {
  owned_fields_.push_back({name, PJ::sdk::ValueRef{value}});
}

void RosParser::addField(const std::string& name, PJ::sdk::ValueRef value) {
  owned_fields_.push_back({name, value});
}

void RosParser::addStringField(const std::string& name, const std::string& value) {
  string_storage_.push_back(value);
  owned_fields_.push_back({name, PJ::sdk::ValueRef{std::string_view(string_storage_.back())}});
}

// ---------------------------------------------------------------------------
// Emit record
// ---------------------------------------------------------------------------

PJ::Status RosParser::emitRecord(PJ::Timestamp ts) {
  named_fields_.clear();
  named_fields_.reserve(owned_fields_.size());
  for (const auto& f : owned_fields_) {
    named_fields_.push_back({.name = f.name, .value = f.value});
  }
  return writeHost().appendRecord(
      ts, PJ::Span<const PJ::sdk::NamedFieldValue>(named_fields_.data(), named_fields_.size()));
}

// ---------------------------------------------------------------------------
// Header helpers
// ---------------------------------------------------------------------------

RosParser::HeaderData RosParser::readHeader() {
  HeaderData h;
  if (!deserializer_->isROS2()) {
    h.seq = deserializer_->deserializeUInt32();
  }
  h.sec = deserializer_->deserializeUInt32();
  h.nsec = deserializer_->deserializeUInt32();

  if (use_embedded_timestamp_) {
    int64_t ts_ns = static_cast<int64_t>(h.sec) * 1000000000LL + static_cast<int64_t>(h.nsec);
    if (ts_ns > 0) {
      current_timestamp_ = ts_ns;
    }
  }

  deserializer_->deserializeString(h.frame_id);
  return h;
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

void RosParser::flattenGeneric(PJ::Span<const uint8_t> payload) {
  try {
    parser_->deserialize(
        RosMsgParser::Span<const uint8_t>(payload.data(), payload.size()), &flat_msg_, deserializer_.get());
  } catch (const std::exception&) {
    // CDR deserialization failed; bail and let the empty owned_fields_
    // signal "no record" to the outer parse() caller. The SDK base
    // class surfaces parse errors via PJ::unexpected() — best-effort
    // mid-flatten errors are silently dropped.
    return;
  }

  // Extract embedded timestamp before field conversion.
  if (use_embedded_timestamp_ && has_header_ && flat_msg_.value.size() >= 2) {
    double ts = 0;
    if (deserializer_->isROS2()) {
      double sec = flat_msg_.value[0].second.convert<double>();
      double nsec = flat_msg_.value[1].second.convert<double>();
      ts = sec + 1e-9 * nsec;
    } else {
      // ROS1: value[1] is stamp (Time builtin)
      ts = flat_msg_.value[1].second.convert<double>();
    }
    if (ts > 0) {
      current_timestamp_ = static_cast<int64_t>(ts * 1e9);
    }
  }

  std::string field_name;
  for (const auto& [key, variant] : flat_msg_.value) {
    key.toStr(field_name);
    if (variant.getTypeID() == RosMsgParser::STRING) {
      auto extracted = variant.extract<std::string>();
      double numeric = 0.0;
      // Apply the string-to-number toggles before falling back to the string
      // representation. parseStringAsDouble returns false when either both
      // toggles are off or no rule matches; in that case we keep the string
      // verbatim. The doubled storage (owned via string_storage_) is needed
      // because ValueRef holds a non-owning string_view.
      if ((boolean_strings_to_number_ || remove_suffix_from_strings_) &&
          parseStringAsDouble(extracted, numeric, remove_suffix_from_strings_, boolean_strings_to_number_)) {
        owned_fields_.push_back({field_name, PJ::sdk::ValueRef{numeric}});
      } else {
        string_storage_.push_back(std::move(extracted));
        owned_fields_.push_back({field_name, PJ::sdk::ValueRef{std::string_view(string_storage_.back())}});
      }
    } else {
      owned_fields_.push_back({field_name, variantToValueRef(variant)});
    }
  }

  addQuaternionRPY();
}

void RosParser::addQuaternionRPY() {
  if (quaternion_prefixes_.empty()) {
    return;
  }

  // Build name → index map for O(1) lookup.
  std::unordered_map<std::string, size_t> idx;
  const size_t n = owned_fields_.size();
  for (size_t i = 0; i < n; i++) {
    idx.emplace(owned_fields_[i].name, i);
  }

  for (const auto& prefix : quaternion_prefixes_) {
    auto find_val = [&](const std::string& suffix) -> double {
      auto it = idx.find(prefix + suffix);
      if (it == idx.end()) {
        return 0.0;
      }
      return valueRefAsDouble(owned_fields_[it->second].value);
    };

    double x = find_val("/x");
    double y = find_val("/y");
    double z = find_val("/z");
    double w = find_val("/w");
    auto rpy = quaternionToRPY(x, y, z, w);
    owned_fields_.push_back({prefix + "/roll", PJ::sdk::ValueRef{rpy.roll}});
    owned_fields_.push_back({prefix + "/pitch", PJ::sdk::ValueRef{rpy.pitch}});
    owned_fields_.push_back({prefix + "/yaw", PJ::sdk::ValueRef{rpy.yaw}});
  }
}

}  // namespace ros_parser_detail
