#pragma once

#include <pj_base/sdk/message_parser_plugin_base.hpp>

#include <nlohmann/json.hpp>
#include <rosx_introspection/ros_parser.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <numbers>
#include <string>
#include <unordered_map>
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
        } else if constexpr (std::is_same_v<T, std::string_view> || std::is_same_v<T, PJ::NullValue> ||
                             std::is_same_v<T, PJ::sdk::TypedNull>) {
          return 0.0;
        } else {
          return static_cast<double>(val);
        }
      },
      v);
}

inline std::pair<double, bool> tryParseDouble(const std::string& s) {
  if (s.empty()) return {0.0, false};
  char* end = nullptr;
  double val = std::strtod(s.c_str(), &end);
  if (end != s.c_str() && *end == '\0') return {val, true};
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
  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override;
  std::string saveConfig() const override;
  PJ::Status loadConfig(std::string_view config_json) override;
  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override;

  enum class Specialization {
    kNone,
    kEmpty,
    kJointState,
    kDiagnosticArray,
    kTFMessage,
    kDataTamerSchemas,
    kDataTamerSnapshot,
    kImu,
    kPose,
    kPoseStamped,
    kOdometry,
    kTransform,
    kTransformStamped,
    kPalStatisticsNames,
    kPalStatisticsValues,
    kTSLDefinition,
    kTSLValues,
  };

  // Configuration
  size_t max_array_size_ = 500;
  bool discard_large_arrays_ = false;
  bool use_embedded_timestamp_ = false;
  bool use_ros1_ = false;
  std::string topic_name_;

  // Schema state
  std::string type_name_;
  Specialization specialization_ = Specialization::kNone;
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
  void ensureDeserializer();
  void detectSpecialization(const std::string& msg_type);
  void detectSchemaFeatures();
  void findQuaternionPrefixes(const RosMsgParser::ROSMessage* msg, const std::string& prefix,
                              const RosMsgParser::RosMessageLibrary& lib);

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

  // Specialized handlers
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
