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

  // Parse state
  std::optional<RosMsgParser::Parser> parser_;
  std::unique_ptr<RosMsgParser::Deserializer> deserializer_;
  RosMsgParser::FlatMessage flat_msg_;
  PJ::Timestamp current_timestamp_ = 0;

  // Output accumulation
  std::vector<FlattenedField> owned_fields_;
  std::vector<PJ::sdk::NamedFieldValue> named_fields_;
  std::deque<std::string> string_storage_;

  // Setup helpers
  PJ::Status compileBoundSchema(bool register_specialized_handler);
  void registerBoundSchemaHandler(const CatalogEntry& entry);
  void ensureDeserializer();
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
    uint32_t sec = 0;
    uint32_t nsec = 0;
    std::string frame_id;
  };

  HeaderData readHeader();
  void emitHeader(const HeaderData& h);

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

  // sensor_msgs/Image
  PJ::Expected<PJ::sdk::ObjectRecord> parseImage(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // sensor_msgs/CompressedImage (also covers compressedDepth via the format string)
  PJ::Expected<PJ::sdk::ObjectRecord> parseCompressedImage(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // sensor_msgs/PointCloud2
  PJ::Expected<PJ::sdk::ObjectRecord> parsePointCloud(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

  // tf2_msgs/TFMessage -> sdk::FrameTransforms (one per TransformStamped, each
  // carrying its own Header.stamp)
  PJ::Expected<PJ::sdk::BuiltinObject> parseFrameTransforms(PJ::Timestamp ts, PJ::sdk::PayloadView payload);

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
    (this->*Handler)();
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
