#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <nlohmann/json.hpp>
#include <numbers>
#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/number_parse.hpp>
#include <pj_laser_scan/laser_scan_projector.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <rosx_introspection/ros_parser.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ros_parser_detail {

// ---------------------------------------------------------------------------
// Quaternion to Roll-Pitch-Yaw conversion
// ---------------------------------------------------------------------------

struct RPY {
  double roll;
  double pitch;
  double yaw;
};

inline RPY quaternionToRPY(double x, double y, double z, double w) {
  double norm2 = w * w + x * x + y * y + z * z;
  if (std::abs(norm2 - 1.0) > std::numeric_limits<double>::epsilon()) {
    double mult = 1.0 / std::sqrt(norm2);
    x *= mult;
    y *= mult;
    z *= mult;
    w *= mult;
  }

  double sinr_cosp = 2 * (w * x + y * z);
  double cosr_cosp = 1 - 2 * (x * x + y * y);
  double roll = std::atan2(sinr_cosp, cosr_cosp);

  double sinp = 2 * (w * y - z * x);
  double pitch = 0.0;
  if (std::abs(sinp) >= 1) {
    pitch = std::copysign(std::numbers::pi / 2.0, sinp);
  } else {
    pitch = std::asin(sinp);
  }

  double siny_cosp = 2 * (w * z + x * y);
  double cosy_cosp = 1 - 2 * (y * y + z * z);
  double yaw = std::atan2(siny_cosp, cosy_cosp);

  return {roll, pitch, yaw};
}

// ---------------------------------------------------------------------------
// Helper types
// ---------------------------------------------------------------------------

struct FlattenedField {
  std::string name;
  PJ::sdk::ValueRef value;
};

inline PJ::sdk::ValueRef variantToValueRef(const RosMsgParser::Variant& variant) {
  using BT = RosMsgParser::BuiltinType;
  switch (variant.getTypeID()) {
    case BT::BOOL:
      return variant.convert<double>() != 0.0;
    case BT::CHAR:
    case BT::INT8:
      return variant.extract<int8_t>();
    case BT::UINT8:
    case BT::BYTE:
      return variant.extract<uint8_t>();
    case BT::INT16:
      return variant.extract<int16_t>();
    case BT::UINT16:
      return variant.extract<uint16_t>();
    case BT::INT32:
      return variant.extract<int32_t>();
    case BT::UINT32:
      return variant.extract<uint32_t>();
    case BT::INT64:
      return variant.extract<int64_t>();
    case BT::UINT64:
      return variant.extract<uint64_t>();
    case BT::FLOAT32:
      return variant.extract<float>();
    case BT::FLOAT64:
      return variant.extract<double>();
    case BT::TIME:
    case BT::DURATION:
      return variant.convert<double>();
    case BT::STRING:
      return PJ::NullValue{};
    default:
      return variant.convert<double>();
  }
}

inline double valueRefAsDouble(const PJ::sdk::ValueRef& v) {
  return std::visit(
      [](const auto& val) -> double {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, bool>) {
          return val ? 1.0 : 0.0;
        } else if constexpr (
            std::is_same_v<T, std::string_view> || std::is_same_v<T, PJ::NullValue> ||
            std::is_same_v<T, PJ::sdk::TypedNull>) {
          return 0.0;
        } else {
          return static_cast<double>(val);
        }
      },
      v);
}

inline std::pair<double, bool> tryParseDouble(const std::string& s) {
  // Strict whole-string parse: succeed only if every byte is consumed.
  // PJ::parseNumber is locale-independent (unlike std::strtod, which respects
  // LC_NUMERIC) and backs the float branch with fast_float.
  if (auto val = PJ::parseNumber<double>(s)) {
    return {*val, true};
  }
  return {0.0, false};
}

/// Absolute nanoseconds -> seconds as a double, keeping the integral second exact.
///
/// A plain `ts_ns * 1e-9` is NOT good enough for a stamp series: an epoch-scale
/// value (~1.8e18 ns) is far beyond 2^53, so the conversion to double quantizes
/// the result to ~200 ns steps — visible the moment anyone zooms into the series
/// this exists to make plottable. Splitting the quotient from the remainder
/// first keeps the seconds exact and the fraction full-resolution. Correct for
/// negative stamps too: integer division and modulo both truncate toward zero in
/// C++, so the two terms carry the same sign and sum to the right value.
inline double nanosecondsToSeconds(int64_t ts_ns) {
  return static_cast<double>(ts_ns / 1000000000LL) + static_cast<double>(ts_ns % 1000000000LL) * 1e-9;
}

inline std::string palStatisticsKey(const std::string& topic) {
  if (topic.size() >= 6 && topic.compare(topic.size() - 6, 6, "/names") == 0) {
    return topic.substr(0, topic.size() - 6);
  }
  if (topic.size() >= 7 && topic.compare(topic.size() - 7, 7, "/values") == 0) {
    return topic.substr(0, topic.size() - 7);
  }
  return topic;
}

// ---------------------------------------------------------------------------
// RosParser
// ---------------------------------------------------------------------------

class RosParser : public PJ::MessageParserPluginBase {
 public:
  /// Default-constructed. The class-level catalog (see catalog() below) holds
  /// the schemas this plugin understands; bindSchema looks the bound type up
  /// in the catalog and registers exactly one SchemaHandler tailored to it
  /// (or a generic flatten handler when the bound type is unknown). No
  /// per-instance handler table populated at construction time.
  RosParser() = default;

  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override;
  std::string saveConfig() const override;
  PJ::Status loadConfig(std::string_view config_json) override;
  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override;

  // ----- Class-level schema catalog -----
  //
  // Pure data, mirrors PJ::sdk::SchemaHandler field naming. Each entry
  // holds member-function pointers describing what this plugin does for a
  // given schema. bindSchema looks the entry up, binds the pointers to
  // `this` via std::bind_front, and registers a single SchemaHandler.
  // Per-instance handlers_ table ends up with one entry.
  //
  // Catalog is uniform: every entry uses the same `parse_scalars` field,
  // whose value is a member-fn-ptr matching SchemaHandler::parse_scalars.
  // For schemas served by an imperative handle*() void method, the entry
  // points directly at the wrapVoidHandler<Handler> template instantiation
  // — that gives a member-fn-ptr of the right shape, no per-entry
  // wrapping logic needed at bind time.
  struct CatalogEntry {
    // Catalog key reserved for the default (catch-all) entry. When
    // bindSchema() looks up a schema name and finds no specific
    // match, it resolves to the entry keyed by this value instead.
    static constexpr const char* kDefault = "*";

    PJ::sdk::BuiltinObjectType object_type = PJ::sdk::BuiltinObjectType::kNone;

    PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> (RosParser::*parse_scalars)(
        PJ::Timestamp, PJ::Span<const uint8_t>) = nullptr;

    PJ::Expected<PJ::sdk::ObjectRecord> (RosParser::*parse_object)(PJ::Timestamp, PJ::sdk::PayloadView) = nullptr;
  };

  static const std::unordered_map<std::string, CatalogEntry>& catalog();

  // Configuration
  size_t max_array_size_ = 500;
  bool discard_large_arrays_ = false;
  bool use_embedded_timestamp_ = false;
  bool use_ros1_ = false;
  // String-to-number conversion toggles applied at the
  // `RosMsgParser::STRING` write path in flattenGeneric.
  bool boolean_strings_to_number_ = false;
  bool remove_suffix_from_strings_ = false;
  std::string topic_name_;

  // Schema state
  std::string type_name_;
  std::string schema_definition_;
  std::string schema_encoding_ = "ros2msg";
  RosMsgParser::SchemaFormat schema_format_ = RosMsgParser::ROS_MSG;
  bool schema_format_configured_ = false;
  bool schema_bound_ = false;
  bool schema_compiled_ = false;
  bool has_header_ = false;
  std::vector<std::string> quaternion_prefixes_;

  // visualization_msgs/Marker layout flags, sniffed from the bound .msg
  // definition in bindSchema. The Marker wire layout gained a texture block
  // (texture_resource / texture / uv_coordinates) and a mesh_file field in
  // ROS 2 humble; EOL foxy/galactic and ROS 1 lack them. These gate the
  // variable-tail decode so it stays aligned on every layout.
  bool marker_has_texture_block_ = false;
  bool marker_has_mesh_file_ = false;

  // Latches the one-time "trailing bytes" warning in parseYoloDetectionArray
  // (#4): a corrupt-but-in-bounds count can desync the positional CDR decode
  // without overrunning, so the try/catch never sees it. We warn once instead
  // of throwing — a strict end-of-buffer check would risk rejecting valid
  // frames on CDR alignment slack (no other handler asserts bytesLeft()==0).
  bool yolo_trailing_warned_ = false;

  // Parse state
  std::optional<RosMsgParser::Parser> parser_;
  std::unique_ptr<RosMsgParser::Deserializer> deserializer_;
  RosMsgParser::FlatMessage flat_msg_;
  PJ::Timestamp current_timestamp_ = 0;

  // LaserScan -> PointCloud projector. One per parser instance (= per topic),
  // so its cos/sin LUT — keyed on (ray_count, angle_min, angle_increment) —
  // is computed once for a whole recording of a fixed scanner config.
  PJ::laser_scan::LaserScanProjector laser_projector_;
  // Reusable parseLaserScan scratch (cleared per call): avoids two per-message
  // heap allocations on the hot path, same pattern as owned_fields_ below.
  std::vector<float> laserscan_ranges_scratch_;
  std::vector<float> laserscan_intensities_scratch_;

  // Output accumulation
  std::vector<FlattenedField> owned_fields_;
  std::vector<PJ::sdk::NamedFieldValue> named_fields_;
  std::deque<std::string> string_storage_;

  // Setup helpers
  PJ::Status compileBoundSchema(bool register_specialized_handler);
  void registerBoundSchemaHandler(const CatalogEntry& entry);
  // Resolve the catalog entry for a bound schema, applying topic-conditional
  // overrides (e.g. std_msgs/String on a robot_description topic -> RobotDescription).
  CatalogEntry selectCatalogEntry(const std::string& msg_type) const;
  void ensureDeserializer();
  // Reads a CDR byte sequence (uint8[] / sequence<uint8>) and validates its
  // wire-declared length against the bytes remaining in the message. NanoCDR's
  // byte-sequence reader trusts the declared length and does not clamp it to the
  // buffer (unlike the ROS1 deserializer and NanoCDR's own string reader): on a
  // truncated/corrupt message it advances the cursor past the end and returns a span
  // longer than the payload. Reading that span — or any field the handler reads
  // after it, since the cursor is now past the end — is an out-of-bounds access.
  // Throws std::runtime_error on overrun (matching the ROS1 reader) so the decode is
  // aborted at once. Every object handler wraps its body in try/catch and the scalar
  // void-handler path is guarded in wrapVoidHandler(), so the throw becomes an
  // Expected error (no frame, no crash), never a std::terminate across the C ABI.
  // Read all bulk uint8[] fields through this, never the deserializer directly.
  RosMsgParser::Span<const uint8_t> readByteSequence();
  void detectSchemaFeatures();
  void findQuaternionPrefixes(
      const RosMsgParser::ROSMessage* msg, const std::string& prefix, const RosMsgParser::RosMessageLibrary& lib);

  // Field accumulation helpers
  void addField(const std::string& name, double value);
  void addField(const std::string& name, PJ::sdk::ValueRef value);
  void addStringField(const std::string& name, const std::string& value);

  // Emit record
  PJ::Status emitRecord(PJ::Timestamp ts);

  // Header helpers
  struct HeaderData {
    uint32_t seq = 0;
    // Seconds since the epoch. Signed and 64-bit so BOTH wire encodings fit
    // losslessly: ROS 2's builtin_interfaces/Time.sec is an int32 (pre-1970 and
    // relative stamps are legal and do occur), while ROS 1's ros::Time.sec is a
    // uint32 whose upper half would overflow an int32.
    int64_t sec = 0;
    uint32_t nsec = 0;
    std::string frame_id;
  };

  HeaderData readHeader();
  void emitHeader(const HeaderData& h);

  // Reads a BARE builtin_interfaces/Time (sec int32, nanosec uint32) at the
  // cursor — the head of the foxglove_msgs schemas, which carry a raw Time
  // instead of a std_msgs/Header — and returns it as absolute nanoseconds.
  // Adopts it as current_timestamp_ under the usual use_embedded_timestamp_ &&
  // ts_ns > 0 contract, so every bare-Time schema behaves like readHeader().
  // `sec` is read SIGNED: builtin_interfaces/Time is a ROS 2 type and its sec
  // field is int32 on every wire that carries it.
  int64_t readBareTime();

  // Composition parse helpers (used by specialization handlers)
  void parseVector3(const std::string& prefix);
  void parsePoint(const std::string& prefix);
  void parseQuaternion(const std::string& prefix);
  template <size_t N>
  void parseCovariance(const std::string& prefix);
  void parseTwist(const std::string& prefix);
  void parsePose(const std::string& prefix);
  void parseTransform(const std::string& prefix);
  void parsePoseWithCovariance(const std::string& prefix);
  void parseTwistWithCovariance(const std::string& prefix);

  // ----- Canonical-object handlers (route: parseScalars / parseObject) -----
  //
  // Each schema maps one ROS canonical type to its sdk::X counterpart via
  // a single parse<X>() entry point that returns a BuiltinObject ready
  // for ObjectStore ingestion. The scalar-side companion is shared across
  // all object-bearing schemas: parseScalarsDiscardingLargeArrays() reuses
  // the generic flattenGeneric path with a forced DISCARD_LARGE_ARRAYS
  // policy, so the bulk byte payload (data[]) is dropped automatically and
  // we keep only the small metadata as scalar columns. No per-schema
  // hand-written scalar walker.

  // Shared scalar-side handler registered by every object-bearing schema.
  PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> parseScalarsDiscardingLargeArrays(
      PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // Default-handler scalar route. Walks any ROS message whose schema is
  // known via rosx_introspection (flattenGeneric) and harvests all fields
  // honoring the user-configured array policy. Registered as the default
  // SchemaHandler in the constructor — fires for every type name not in
  // the specific table.
  PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> parseScalarsGeneric(
      PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // Slim scalar route for object-only schemas (markers) that carry no
  // meaningful scalar columns. Returns an EMPTY field set: enough for the
  // host's eager-scalar ingest path (parse() returns ok on empty fields) to
  // succeed so the canonical object still reaches the ObjectStore under ANY
  // ObjectIngestPolicy — not only kPureLazy. Without a parse_scalars, an
  // object-only handler makes parseScalars fail and the message push aborts on
  // non-kPureLazy policies (e.g. live streaming sources, which set no override).
  PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> parseScalarsObjectOnly(
      PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // ----- Slim direct-CDR scalar routes -----
  //
  // Shared prologue for every hand-written scalar route that reads the CDR wire
  // directly (no rosx_introspection flatten): resets the field accumulators —
  // emitHeader()/addStringField() write into owned_fields_ + string_storage_ —
  // seeds current_timestamp_ with the host time, and points the deserializer at
  // the payload. Mirrors the setup half of wrapVoidHandler().
  void beginDirectScalarRead(PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // Copies owned_fields_ into the vector shape parse_scalars returns. The
  // string_view values keep pointing into string_storage_, which stays alive
  // until the next parse on this instance — the same lifetime contract
  // parseScalarsGeneric and wrapVoidHandler already rely on.
  std::vector<PJ::sdk::NamedFieldValue> harvestOwnedFields() const;

  // Header-only scalar route: emits just the emitHeader() series. For
  // object-bearing schemas that lead with a std_msgs/Header but carry nothing
  // else worth plotting — visualization_msgs/Marker and
  // point_cloud_interfaces/CompressedPointCloud2 (whose blob is compressed, so
  // no point count can be measured from it).
  PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> parseHeaderOnlyScalars(
      PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // sensor_msgs/PointCloud2: the header series plus /num_points, measured as
  // len(data) / point_step — the points actually present, NOT the width*height
  // the producer declares. A full introspection flatten of a cloud is worthless
  // as time series (the bulk data[] is dropped anyway) and costs an entire
  // schema walk per message.
  PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> parsePointCloud2Scalars(
      PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // foxglove_msgs/CompressedPointCloud: leads with a BARE
  // builtin_interfaces/Time (NOT a std_msgs/Header — readHeader() must not be
  // used, its ROS1 seq branch would consume the wrong word), then frame_id.
  // Emits /timestamp + /frame_id; the wire carries no point count (the cloud is
  // an opaque compressed blob), so there is nothing else to emit.
  PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> parseFoxgloveCompressedPointCloudScalars(
      PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // sensor_msgs/Image
  PJ::Expected<PJ::sdk::ObjectRecord> parseImage(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // sensor_msgs/CompressedImage (also covers compressedDepth via the format string)
  PJ::Expected<PJ::sdk::ObjectRecord> parseCompressedImage(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // sensor_msgs/CameraInfo -> sdk::CameraInfo (intrinsics + distortion). Lets the
  // 2D image view rectify frames and align annotation overlays.
  PJ::Expected<PJ::sdk::ObjectRecord> parseCameraInfo(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // foxglove_msgs/CompressedVideo -> sdk::VideoFrame (zero-copy compressed bitstream)
  PJ::Expected<PJ::sdk::ObjectRecord> parseCompressedVideo(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // sensor_msgs/PointCloud2
  PJ::Expected<PJ::sdk::ObjectRecord> parsePointCloud(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // sensor_msgs/LaserScan -> sdk::PointCloud, eagerly projected (owned point
  // buffer) through laser_projector_'s cached cos/sin LUT.
  PJ::Expected<PJ::sdk::ObjectRecord> parseLaserScan(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // foxglove_msgs/CompressedPointCloud -> sdk::CompressedPointCloud (zero-copy compressed blob)
  PJ::Expected<PJ::sdk::ObjectRecord> parseFoxgloveCompressedPointCloud(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // point_cloud_interfaces/CompressedPointCloud2 -> sdk::CompressedPointCloud (zero-copy compressed blob)
  PJ::Expected<PJ::sdk::ObjectRecord> parseCompressedPointCloud2(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // tf2_msgs/TFMessage -> sdk::FrameTransforms (one per TransformStamped, each
  // carrying its own Header.stamp)
  PJ::Expected<PJ::sdk::ObjectRecord> parseFrameTransforms(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // geometry_msgs/TransformStamped -> sdk::FrameTransforms (single element)
  PJ::Expected<PJ::sdk::ObjectRecord> parseTransformStampedObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // geometry_msgs/PoseArray -> sdk::PosesInFrame (poses only; rendering style is
  // a viewer-side concern).
  PJ::Expected<PJ::sdk::ObjectRecord> parsePoseArray(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // foxglove_msgs/PosesInFrame -> sdk::PosesInFrame. Leads with a BARE
  // builtin_interfaces/Time (not a Header), then frame_id, then the poses.
  PJ::Expected<PJ::sdk::ObjectRecord> parseFoxglovePosesInFrame(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // geometry_msgs/PoseStamped -> sdk::PosesInFrame (single element). Dual route:
  // keeps its per-axis scalar flatten (handlePoseStamped) alongside the object.
  PJ::Expected<PJ::sdk::ObjectRecord> parsePoseStampedObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // nav_msgs/Odometry -> sdk::PosesInFrame (single element: the pose in the
  // header frame). Dual route: keeps its per-axis scalar flatten
  // (handleOdometry) alongside the object.
  PJ::Expected<PJ::sdk::ObjectRecord> parseOdometryObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // nav_msgs/Path -> sdk::PosesInFrame. The object takes the PATH header's
  // frame_id + stamp; each PoseStamped's own header is read and dropped (a
  // PosesInFrame is one frame at one instant).
  PJ::Expected<PJ::sdk::ObjectRecord> parsePath(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // nav_msgs/OccupancyGrid -> sdk::OccupancyGrid (byte-backed, zero-copy cells)
  PJ::Expected<PJ::sdk::ObjectRecord> parseOccupancyGrid(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // map_msgs/OccupancyGridUpdate -> sdk::OccupancyGridUpdate (byte-backed, zero-copy cells)
  PJ::Expected<PJ::sdk::ObjectRecord> parseOccupancyGridUpdate(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // std_msgs/String on a robot_description topic -> sdk::RobotDescription
  PJ::Expected<PJ::sdk::ObjectRecord> parseRobotDescription(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // visualization_msgs/Marker -> sdk::SceneEntities (one SceneEntity, or one
  // SceneEntityDeletion for DELETE/DELETEALL).
  PJ::Expected<PJ::sdk::ObjectRecord> parseMarker(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // visualization_msgs/MarkerArray -> sdk::SceneEntities (one per Marker).
  PJ::Expected<PJ::sdk::ObjectRecord> parseMarkerArray(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // yolo_msgs/DetectionArray -> sdk::ImageAnnotations (boxes, labels, mask
  // outline, 2D keypoints). Third-party message, net-new — see YOLO_NOTES.md.
  PJ::Expected<PJ::sdk::ObjectRecord> parseYoloDetectionArray(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // Slim scalar companion for yolo_msgs/DetectionArray: emits `num_detections`
  // so the object ingests under ANY policy (see parseScalarsObjectOnly) and
  // gives a useful plottable count. parse_object is unchanged.
  PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> parseYoloScalars(
      PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // Reads one visualization_msgs/Marker from the deserializer at the current
  // cursor and appends the resulting entity (ADD/MODIFY) or deletion
  // (DELETE/DELETEALL) to `out`. Always consumes the whole marker so the next
  // element of a MarkerArray stays aligned. Shared by parseMarker / parseMarkerArray.
  void decodeOneMarker(PJ::sdk::SceneEntities& out);

  // Reads one geometry_msgs/TransformStamped from the deserializer into a
  // FrameTransform. Shared by parseFrameTransforms and parseTransformStampedObject.
  PJ::sdk::FrameTransform readStampedTransform();

  // Reads one geometry_msgs/Pose (7 float64: position xyz + orientation xyzw)
  // from the deserializer at the current cursor. Shared by all the PosesInFrame
  // object handlers (PoseArray / foxglove PosesInFrame / PoseStamped / Path).
  PJ::sdk::Pose readPose();

  // ----- Specialized scalar handlers -----
  //
  // Each one walks a specific ROS message type and pushes its decoded fields
  // into owned_fields_ via addField/addStringField. They are imperative
  // (return void, side-effect on the parser's accumulators).
  //
  // wrapVoidHandler<Handler> is a member-function template that decorates
  // such a handler with the standard setup + harvest boilerplate, yielding
  // a member function whose signature matches PJ::sdk::SchemaHandler
  // ::parse_scalars exactly. Each instantiation has its own member-fn-ptr
  // address: write `&RosParser::wrapVoidHandler<&RosParser::handleImu>`
  // and you have a parse_scalars callable suitable for direct catalog
  // registration.
  template <void (RosParser::*Handler)()>
  PJ::Expected<std::vector<PJ::sdk::NamedFieldValue>> wrapVoidHandler(
      PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
    if (!parser_.has_value()) {
      return PJ::unexpected(std::string("no schema bound"));
    }
    ensureDeserializer();
    owned_fields_.clear();
    string_storage_.clear();
    named_fields_.clear();
    current_timestamp_ = ts;
    deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.data(), payload.size()));
    // The object handlers each carry their own try/catch; the scalar void handlers
    // do not, and this wrapper is the C-ABI-facing entry for them. A CDR decode
    // throw (e.g. readByteSequence / deserializeString on a truncated message) must
    // become an Expected error here, never propagate into the noexcept trampoline.
    try {
      (this->*Handler)();
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("ROS scalar decode error: ") + e.what());
    }
    std::vector<PJ::sdk::NamedFieldValue> out;
    out.reserve(owned_fields_.size());
    for (const auto& f : owned_fields_) {
      out.push_back({.name = f.name, .value = f.value});
    }
    return out;
  }

  void handleEmpty();
  void handlePose();
  void handlePoseStamped();
  void handleTransform();
  void handleTransformStamped();
  void handleImu();
  void handleOdometry();
  void handleJointState();
  void handleDiagnosticArray();
  void handleTFMessage();
  void handleDataTamerSchemas();
  void handleDataTamerSnapshot();
  void handlePalStatisticsNames();
  void handlePalStatisticsValues();
  void handleTSLDefinition();
  void handleTSLValues();

  // Generic path
  void flattenGeneric(PJ::Span<const uint8_t> payload);
  void addQuaternionRPY();
};

// parseCovariance is a template — define it here.
template <size_t N>
void RosParser::parseCovariance(const std::string& prefix) {
  std::array<double, N * N> cov{};
  for (auto& val : cov) {
    val = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  }
  for (size_t i = 0; i < N; i++) {
    for (size_t j = i; j < N; j++) {
      size_t index = i * N + j;
      std::string name = prefix + "/[" + std::to_string(i) + ";" + std::to_string(j) + "]";
      addField(name, cov[index]);
    }
  }
}

}  // namespace ros_parser_detail
