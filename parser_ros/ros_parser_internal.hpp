#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <nlohmann/json.hpp>
#include <numbers>
#include <optional>
#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/number_parse.hpp>
#include <pj_grid_map/grid_map_transcoder.hpp>
#include <pj_laser_scan/laser_scan_projector.hpp>
#include <pj_plugins/sdk/message_parser_plugin_base.hpp>
#include <rosx_introspection/ros_parser.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bound_field_cache.hpp"

namespace ros_parser_detail {

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

/// Appends a segment to a field-name buffer for one scope, and truncates the
/// buffer back to its previous size when the scope ends (also on a throw).
class PathSegment {
 public:
  PathSegment(std::string& path, std::string_view segment) : path_(path), size_(path.size()) {
    path_.append(segment);
  }
  ~PathSegment() {
    path_.resize(size_);
  }
  PathSegment(const PathSegment&) = delete;
  PathSegment& operator=(const PathSegment&) = delete;

 private:
  std::string& path_;
  size_t size_;
};

/// The fields of the message being parsed, in the shape the write host takes.
/// Slots, name strings and text storage are reused from message to message, so a
/// steady stream allocates nothing here. String values point into `text_`, a deque
/// (it never moves an element), and stay valid until the next clear().
class FieldBuffer {
 public:
  void clear() {
    size_ = 0;
    text_size_ = 0;
  }
  /// A name id never handed out before. An id names exactly one string: the
  /// caller that takes one never changes the string it stands for.
  [[nodiscard]] uint64_t newNameId() {
    return ++last_name_id_;
  }
  /// Every slot's name carries such an id, which lets BoundFieldCache skip
  /// comparing names (nameIds()). `name_id`, when not 0, is the caller's id for
  /// `name` (from newNameId()): a slot that holds that id already holds that
  /// name, so it is neither compared nor copied. Otherwise the name is copied
  /// only when it differs from the slot's, and a slot whose name changed without
  /// a caller id gets a new id.
  void add(std::string_view name, PJ::sdk::ValueRef value, uint64_t name_id = 0) {
    if (size_ == fields_.size()) {
      // name_ids_ grows first and to an exact size, so a throw at either step
      // leaves it at least as long as fields_, and the next call completes it.
      name_ids_.resize(size_ + 1);
      fields_.emplace_back();
    }
    auto& field = fields_[size_];
    auto& slot_id = name_ids_[size_];
    if (name_id == 0 || slot_id != name_id) {
      const bool renamed = slot_id == 0 || field.name != name;
      if (renamed) {
        field.name.assign(name);
      }
      if (name_id != 0) {
        slot_id = name_id;
      } else if (renamed) {
        slot_id = newNameId();
      }
    }
    field.value = value;
    ++size_;
  }
  void addString(std::string_view name, std::string_view text, uint64_t name_id = 0) {
    if (text_size_ == text_.size()) {
      text_.emplace_back();
    }
    auto& stored = text_[text_size_++];
    stored.assign(text);
    add(name, std::string_view(stored), name_id);
  }
  void setValue(size_t i, PJ::sdk::ValueRef value) {
    fields_[i].value = value;
  }
  [[nodiscard]] bool empty() const {
    return size_ == 0;
  }
  [[nodiscard]] size_t size() const {
    return size_;
  }
  [[nodiscard]] const PJ::sdk::NamedFieldValue& operator[](size_t i) const {
    return fields_[i];
  }
  [[nodiscard]] const PJ::sdk::NamedFieldValue* begin() const {
    return fields_.data();
  }
  [[nodiscard]] const PJ::sdk::NamedFieldValue* end() const {
    return fields_.data() + size_;
  }
  [[nodiscard]] PJ::Span<const PJ::sdk::NamedFieldValue> span() const {
    return {fields_.data(), size_};
  }
  [[nodiscard]] PJ::Span<const uint64_t> nameIds() const {
    return {name_ids_.data(), size_};
  }

 private:
  std::vector<PJ::sdk::NamedFieldValue> fields_;
  std::vector<uint64_t> name_ids_;  ///< parallel to fields_ (at least as long); 0 = slot never named
  uint64_t last_name_id_ = 0;
  size_t size_ = 0;
  std::deque<std::string> text_;
  size_t text_size_ = 0;
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
  // Whole-string parse; locale-independent, unlike std::strtod (LC_NUMERIC).
  if (auto val = PJ::parseNumber<double>(s)) {
    return {*val, true};
  }
  return {0.0, false};
}

/// Absolute nanoseconds -> seconds as a double, keeping the integral second exact.
///
/// A plain `ts_ns * 1e-9` quantizes an epoch-scale stamp (~1.8e18 ns, beyond 2^53)
/// to ~200 ns steps. Splitting quotient and remainder keeps the seconds exact; both
/// truncate toward zero, so negative stamps sum correctly too.
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

class RosParser : public PJ::MessageParserPluginBase {
 public:
  /// bindSchema looks the bound type up in catalog() and registers exactly one
  /// SchemaHandler for it (the generic flatten handler for an unknown type).
  RosParser() = default;

  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override;
  std::string saveConfig() const override;
  PJ::Status loadConfig(std::string_view config_json) override;
  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override;

  // Per-schema member-function pointers, named after PJ::sdk::SchemaHandler.
  // bindSchema binds them to `this` with std::bind_front. A schema served by an
  // imperative handle*() void method points `parse_scalars` at the matching
  // wrapVoidHandler<Handler> instantiation, which has the right signature.
  struct CatalogEntry {
    // Catalog key of the catch-all entry bindSchema falls back to.
    static constexpr const char* kDefault = "*";

    PJ::sdk::BuiltinObjectType object_type = PJ::sdk::BuiltinObjectType::kNone;

    PJ::Status (RosParser::*parse_scalars)(PJ::Timestamp, PJ::Span<const uint8_t>) = nullptr;

    PJ::Expected<PJ::sdk::ObjectRecord> (RosParser::*parse_object)(PJ::Timestamp, PJ::sdk::PayloadView) = nullptr;
  };

  static const std::unordered_map<std::string, CatalogEntry>& catalog();

  size_t max_array_size_ = 500;
  bool discard_large_arrays_ = false;
  bool use_embedded_timestamp_ = false;
  bool use_ros1_ = false;
  // String-to-number conversions at the `RosMsgParser::STRING` write path in flattenGeneric.
  bool boolean_strings_to_number_ = false;
  bool remove_suffix_from_strings_ = false;
  std::string topic_name_;
  std::string pal_statistics_key_;  ///< palStatisticsKey(topic_name_)

  std::string type_name_;
  std::string schema_definition_;
  std::string schema_encoding_ = "ros2msg";
  RosMsgParser::SchemaFormat schema_format_ = RosMsgParser::ROS_MSG;
  bool schema_format_configured_ = false;
  bool schema_bound_ = false;
  bool schema_compiled_ = false;
  bool has_header_ = false;
  // One geometry_msgs/Quaternion of the generic walk (not inside an array): the
  // names its roll/pitch/yaw are computed from and written under, built once per
  // schema, and where each component was found in the previous message.
  struct QuaternionLeaves {
    std::array<std::string, 4> component;  ///< <prefix>/x, /y, /z, /w
    std::array<std::string, 3> angle;      ///< <prefix>/roll, /pitch, /yaw
    std::array<uint64_t, 3> angle_id{};    ///< FieldBuffer name ids of `angle`
    std::array<size_t, 4> index{};         ///< position of each component last time; checked by name
  };
  std::vector<QuaternionLeaves> quaternions_;

  // visualization_msgs/Marker layout flags, sniffed from the bound .msg in bindSchema.
  // ROS 2 humble and later carry a texture block (texture_resource / texture /
  // uv_coordinates) and a mesh_file field; foxy/galactic and ROS 1 do not. They
  // gate the variable-tail decode so it stays aligned on every layout.
  bool marker_has_texture_block_ = false;
  bool marker_has_mesh_file_ = false;

  // diagnostic_msgs/DiagnosticStatus: the order of its four leading fields as the
  // bound schema declares them, read in compileBoundSchema. The standard order is
  // level, name, message, hardware_id; Waymo's MCAP converter writes name first.
  // Empty for a layout handleDiagnosticArray cannot follow: the generic walker binds.
  enum class DiagField : uint8_t { kLevel, kName, kMessage, kHardwareId };
  using DiagOrder = std::array<DiagField, 4>;
  std::optional<DiagOrder> diag_order_ =
      DiagOrder{DiagField::kLevel, DiagField::kName, DiagField::kMessage, DiagField::kHardwareId};

  // Latches the one-time "trailing bytes" warning in parseYoloDetectionArray: a
  // corrupt-but-in-bounds count can desync the positional CDR decode without
  // overrunning, so the try/catch never sees it. It warns instead of throwing
  // because a strict end-of-buffer check would reject valid frames on CDR
  // alignment slack (no other handler asserts bytesLeft()==0).
  bool yolo_trailing_warned_ = false;

  std::optional<RosMsgParser::Parser> parser_;
  std::unique_ptr<RosMsgParser::Deserializer> deserializer_;
  PJ::Timestamp current_timestamp_ = 0;

  // One per parser instance (= per topic): its cos/sin LUT, keyed on (ray_count,
  // angle_min, angle_increment), is computed once per fixed scanner config.
  PJ::laser_scan::LaserScanProjector laser_projector_;
  // parseLaserScan scratch, reused across messages.
  std::vector<float> laserscan_ranges_scratch_;
  std::vector<float> laserscan_intensities_scratch_;
  // grid_map_msgs/GridMap scratch: one float vector per layer, reused across messages.
  std::vector<std::vector<float>> gridmap_layer_scratch_;

  FieldBuffer fields_;
  // The scalar route of the bound schema; parse() calls it directly and appends
  // fields_ as they are, without the copy the functional parseScalars returns.
  PJ::Status (RosParser::*scalar_route_)(PJ::Timestamp, PJ::Span<const uint8_t>) = nullptr;
  // Direct ingest (parse / emitRecord) appends by handle through this cache.
  BoundFieldCache bound_field_cache_;

  std::optional<DiagOrder> readDiagnosticLayout() const;
  PJ::Status compileBoundSchema(bool register_specialized_handler);
  void registerBoundSchemaHandler(const CatalogEntry& entry);
  // Resolve the catalog entry for a bound schema, applying topic-conditional
  // overrides (e.g. std_msgs/String on a robot_description topic -> RobotDescription).
  CatalogEntry selectCatalogEntry(const std::string& msg_type) const;
  void ensureDeserializer();
  // Reads a CDR byte sequence (uint8[]) and checks its declared length against the
  // bytes left. NanoCDR's byte-sequence reader trusts the declared length (the ROS1
  // deserializer and NanoCDR's string reader clamp it): on a truncated/corrupt
  // message the cursor moves past the end, the returned span outruns the payload and
  // every later read is out of bounds. Throws std::runtime_error on overrun; object
  // handlers catch it and wrapVoidHandler() guards the scalar path, so it becomes an
  // Expected error, never a std::terminate across the C ABI. Read all bulk uint8[]
  // fields through this, never the deserializer directly.
  RosMsgParser::Span<const uint8_t> readByteSequence();
  void detectSchemaFeatures();
  void findQuaternionPrefixes(
      const RosMsgParser::ROSMessage* msg, const std::string& prefix, const RosMsgParser::RosMessageLibrary& lib);

  void addField(std::string_view name, double value);
  void addField(std::string_view name, PJ::sdk::ValueRef value);
  void addStringField(std::string_view name, std::string_view value);

  PJ::Status emitRecord(PJ::Timestamp ts);

  struct HeaderData {
    uint32_t seq = 0;
    // Seconds since the epoch. Signed 64-bit to fit both wire encodings: ROS 2's
    // Time.sec is an int32 (pre-1970 and relative stamps occur), ROS 1's is a uint32.
    int64_t sec = 0;
    uint32_t nsec = 0;
    std::string frame_id;
  };

  // Valid until the next readHeader(); copy it to keep it longer.
  const HeaderData& readHeader();
  HeaderData header_;
  // Per-message scratch of the specialized handlers, reused for its capacity.
  std::string child_frame_id_;
  std::vector<std::string> joint_names_;
  std::vector<double> joint_positions_;
  std::vector<double> joint_velocities_;
  std::vector<double> joint_efforts_;
  std::string diag_name_;
  std::string diag_message_text_;
  std::string diag_hardware_id_;
  std::string diag_key_;
  std::string diag_value_;
  // DiagnosticArray name -> {message it was last seen in, times seen there}.
  std::unordered_map<std::string, std::pair<uint64_t, int>> diag_seen_;
  uint64_t diag_message_ = 0;
  std::vector<double> statistics_values_;  ///< PAL / TSL values of one message
  // Reads an array length and checks it against the bytes left, given the
  // smallest wire size of one element, before anything is sized from it:
  // scratch vectors live as long as the parser, so a corrupt count must not
  // stay resident. Throws std::runtime_error (a decode error) on overrun.
  size_t readArrayCount(size_t min_element_wire_size);
  void emitHeader(const HeaderData& h);

  // Reads a BARE stamp (sec, nsec) at the cursor, the head of the foxglove_msgs
  // schemas (a raw time instead of a std_msgs/Header), and returns absolute
  // nanoseconds. Adopts it as current_timestamp_ under the same
  // use_embedded_timestamp_ && ts_ns > 0 contract as readHeader(). Same sec
  // signedness split as HeaderData: int32 on ROS 2, uint32 on ROS 1.
  int64_t readBareTime();

  // Composition helpers of the specialized handlers. Field names are built in the
  // reused buffer path_: each helper appends `segment` for its own fields and
  // truncates it on return (PathSegment). A handler sets path_ to its prefix
  // (usually empty) once, then calls them.
  void addFieldAt(std::string_view segment, double value);
  void parseVector3(std::string_view segment);
  void parsePoint(std::string_view segment);
  void parseQuaternion(std::string_view segment);
  template <size_t N>
  void parseCovariance(std::string_view segment);
  void parseTwist(std::string_view segment);
  void parsePose(std::string_view segment);
  // Emits an already-read Pose under `segment` exactly as parsePose does
  // (position x/y/z, orientation x/y/z/w + roll/pitch/yaw).
  void emitPose(std::string_view segment, const PJ::sdk::Pose& pose);
  void parseTransform(std::string_view segment);
  void parsePoseWithCovariance(std::string_view segment);
  void parseTwistWithCovariance(std::string_view segment);
  std::string path_;  ///< field-name buffer of the specialized handlers

  // Canonical-object handlers: each parse<X>() returns the object for one ROS schema.
  // The scalar side of most is parseScalarsDiscardingLargeArrays(): the generic
  // flatten with DISCARD_LARGE_ARRAYS forced, so the bulk data[] is dropped and only
  // the small metadata stays as columns.
  PJ::Status parseScalarsDiscardingLargeArrays(PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // Default scalar route: flattenGeneric over any schema rosx_introspection knows,
  // honoring the configured array policy. Serves every type not in the catalog.
  PJ::Status parseScalarsGeneric(PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // Slim scalar route for object-only schemas (markers): returns an EMPTY field set.
  // Without a parse_scalars, parseScalars fails and the push aborts on non-kPureLazy
  // ingest policies (e.g. live sources, which set no override); with it the object
  // reaches the ObjectStore under any policy.
  PJ::Status parseScalarsObjectOnly(PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // Shared prologue of the hand-written scalar routes that read the CDR wire directly:
  // clears fields_, seeds current_timestamp_ with the host time and points the
  // deserializer at the payload. The setup half of wrapVoidHandler().
  void beginDirectScalarRead(PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // Emits just the emitHeader() series, for object-bearing schemas that lead with a
  // std_msgs/Header and carry nothing else worth plotting: visualization_msgs/Marker
  // and point_cloud_interfaces/CompressedPointCloud2 (compressed blob, no point count).
  PJ::Status parseHeaderOnlyScalars(PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // sensor_msgs/PointCloud2: the header series plus /num_points = len(data) / point_step,
  // the points actually present, NOT the width*height the producer declares. Avoids a
  // full schema walk per message for a flatten whose bulk data[] is dropped anyway.
  PJ::Status parsePointCloud2Scalars(PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // foxglove_msgs/CompressedPointCloud: leads with a BARE builtin_interfaces/Time, not a
  // std_msgs/Header (readHeader() would misread the word its ROS1 seq branch consumes),
  // then frame_id. Emits /timestamp + /frame_id only: the compressed blob has no point count.
  PJ::Status parseFoxgloveCompressedPointCloudScalars(PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

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

  // rcl_interfaces/Log (ROS 2) and rosgraph_msgs/Log (ROS 1) -> sdk::Log. Dual
  // route: keeps the generic scalar flatten alongside the object.
  PJ::Expected<PJ::sdk::ObjectRecord> parseLogObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // foxglove_msgs/Log, mavros_msgs/StatusText, px4_msgs/LogMessage and
  // px4_msgs/MavlinkLog -> sdk::Log. Same dual route as parseLogObject.
  PJ::Expected<PJ::sdk::ObjectRecord> parseFoxgloveLogObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload);
  PJ::Expected<PJ::sdk::ObjectRecord> parseStatusTextLogObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload);
  PJ::Expected<PJ::sdk::ObjectRecord> parsePx4LogMessageObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload);
  PJ::Expected<PJ::sdk::ObjectRecord> parsePx4MavlinkLogObject(PJ::Timestamp ts, PJ::sdk::PayloadView payload);
  PJ::Expected<PJ::sdk::ObjectRecord> parsePx4Log(PJ::Timestamp ts, PJ::sdk::PayloadView payload, bool severity_first);

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

  // grid_map_msgs/GridMap -> sdk::GridMap, transcoded through pj_grid_map (the
  // wire is a column-major ring buffer per layer; the canonical grid is
  // row-major interleaved records, so the bytes are newly generated and owned).
  PJ::Expected<PJ::sdk::ObjectRecord> parseGridMap(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // Slim scalar companion for grid_map_msgs/GridMap: header series, the
  // GridMapInfo scalars (resolution, lengths, pose), /num_layers and the
  // /size_x, /size_y read from the first layer's layout. The float arrays are
  // never walked.
  PJ::Status parseGridMapScalars(PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // foxglove_msgs/Grid -> sdk::GridMap (zero-copy view of data[], anchored to
  // the payload). Leads with a BARE time (readBareTime), not a Header.
  PJ::Expected<PJ::sdk::ObjectRecord> parseFoxgloveGrid(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // Slim scalar companion for foxglove_msgs/Grid: /timestamp, /frame_id,
  // /column_count, /row_count (derived), strides and /data_size.
  PJ::Status parseFoxgloveGridScalars(PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

  // Reads the whole grid_map_msgs/GridMap body after the deserializer has
  // been positioned at the start of the message. `center` receives the full
  // info.pose (only its x/y reach `out`). Layer floats land in
  // gridmap_layer_scratch_, which `out` borrows, unless `skip_layer_data` is
  // set (the scalar route: the cursor jumps past them and `out.data[k].data`
  // stays empty). Only each layer's `[data_offset, data_offset + cells)`
  // window is copied, so `out.data[k].data_offset` is always 0.
  // `little_endian_wire` (ROS 1, or a little-endian CDR encapsulation) enables
  // the bulk copy; a big-endian CDR wire goes through the byte-swapping
  // per-element read. Throws on a truncated or over-sized wire.
  HeaderData readGridMapMessage(
      PJ::grid_map::GridMapMessage& out, PJ::sdk::Pose& center, bool skip_layer_data, bool little_endian_wire);

  // Reads a foxglove_msgs/Grid body (bare time, frame_id, pose, layout,
  // fields[], data[]) into a GridMap view over `anchor`'s buffer and
  // finalizes it (row_count derivation + validation).
  PJ::Expected<PJ::sdk::GridMap> readFoxgloveGrid(PJ::sdk::BufferAnchor anchor);

  // std_msgs/String on a robot_description topic -> sdk::RobotDescription
  PJ::Expected<PJ::sdk::ObjectRecord> parseRobotDescription(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // visualization_msgs/Marker -> sdk::SceneEntities (one SceneEntity, or one
  // SceneEntityDeletion for DELETE/DELETEALL).
  PJ::Expected<PJ::sdk::ObjectRecord> parseMarker(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // visualization_msgs/MarkerArray -> sdk::SceneEntities (one per Marker).
  PJ::Expected<PJ::sdk::ObjectRecord> parseMarkerArray(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // yolo_msgs/DetectionArray -> sdk::ImageAnnotations (boxes, labels, mask
  // outline, 2D keypoints). Third-party message; see YOLO_NOTES.md.
  PJ::Expected<PJ::sdk::ObjectRecord> parseYoloDetectionArray(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // Slim scalar companion for yolo_msgs/DetectionArray: emits `num_detections`, so the
  // object ingests under ANY policy (see parseScalarsObjectOnly).
  PJ::Status parseYoloScalars(PJ::Timestamp ts, PJ::Span<const uint8_t> payload);

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

  // Specialized scalar handlers: each walks one ROS message type and pushes its fields
  // into fields_ via addField/addStringField. wrapVoidHandler<Handler> adds the setup
  // boilerplate and yields a member function with the SchemaHandler::parse_scalars
  // signature, e.g. `&RosParser::wrapVoidHandler<&RosParser::handleImu>`.
  template <void (RosParser::*Handler)()>
  PJ::Status wrapVoidHandler(PJ::Timestamp ts, PJ::Span<const uint8_t> payload) {
    if (!parser_.has_value()) {
      return PJ::unexpected(std::string("no schema bound"));
    }
    ensureDeserializer();
    fields_.clear();
    current_timestamp_ = ts;
    // The object handlers each carry their own try/catch; the scalar void handlers
    // do not, and this wrapper is the C-ABI-facing entry for them. A CDR decode
    // throw (e.g. readByteSequence / deserializeString on a truncated message) must
    // become an Expected error here, never propagate into the noexcept trampoline.
    try {
      deserializer_->init(RosMsgParser::Span<const uint8_t>(payload.data(), payload.size()));
      (this->*Handler)();
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("ROS scalar decode error: ") + e.what());
    }
    return PJ::okStatus();
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

  // Generic path. An error when the payload does not deserialize against the bound schema.
  PJ::Status flattenGeneric(PJ::Span<const uint8_t> payload);
  // Name of the value at walk position `index`. Rendered only when `leaf`
  // (schema node + array indexes + @key suffixes) differs from the leaf seen
  // at that position in the previous message; otherwise the cached name.
  struct CachedLeaf {
    RosMsgParser::FieldLeaf leaf;
    std::string name;
    uint64_t name_id = 0;  ///< fields_.newNameId() at every rendering, so it names this exact string
  };
  const CachedLeaf& leafName(size_t index, const RosMsgParser::FieldLeaf& leaf);
  std::vector<CachedLeaf> leaf_names_;  ///< by walk position; cleared with each new parser_
  void addQuaternionRPY();
};

template <size_t N>
void RosParser::parseCovariance(std::string_view segment) {
  static_assert(N <= 10, "cell names are built from single-digit indexes");
  std::array<double, N * N> cov{};
  for (auto& val : cov) {
    val = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  }
  PathSegment matrix(path_, segment);
  for (size_t i = 0; i < N; i++) {
    for (size_t j = i; j < N; j++) {
      // "/[i;j]": the upper triangle, row-major.
      const std::array<char, 6> cell = {'/', '[', static_cast<char>('0' + i), ';', static_cast<char>('0' + j), ']'};
      addFieldAt(std::string_view(cell.data(), cell.size()), cov[i * N + j]);
    }
  }
}

}  // namespace ros_parser_detail
