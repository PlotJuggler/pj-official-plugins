#include <pj_base/sdk/message_parser_plugin_base.hpp>

#include <nlohmann/json.hpp>
#include <rosx_introspection/ros_parser.hpp>

#include <data_tamer_parser/data_tamer_parser.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <queue>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Thread-local cross-message state (shared between parser instances)
// ---------------------------------------------------------------------------

thread_local std::unordered_map<uint64_t, DataTamerParser::Schema> g_data_tamer_schemas;

// PAL Statistics: outer key = stripped topic prefix, inner key = names_version
thread_local std::unordered_map<std::string, std::unordered_map<uint32_t, std::vector<std::string>>>
    g_pal_statistics_names;

// TSL: keyed by definition hash
thread_local std::unordered_map<uint64_t, std::vector<std::string>> g_tsl_definitions;
thread_local std::unordered_map<uint64_t, std::queue<std::tuple<int64_t, std::vector<double>>>>
    g_tsl_values_buffer;

// TSL type order (matches old parser)
constexpr std::array<RosMsgParser::BuiltinType, 11> kTslTypeOrder = {
    RosMsgParser::BOOL,    RosMsgParser::INT8,    RosMsgParser::UINT8,
    RosMsgParser::INT16,   RosMsgParser::UINT16,  RosMsgParser::INT32,
    RosMsgParser::UINT32,  RosMsgParser::INT64,   RosMsgParser::UINT64,
    RosMsgParser::FLOAT32, RosMsgParser::FLOAT64,
};

// ---------------------------------------------------------------------------
// Quaternion to Roll-Pitch-Yaw conversion
// ---------------------------------------------------------------------------

struct RPY {
  double roll;
  double pitch;
  double yaw;
};

RPY quaternionToRPY(double x, double y, double z, double w) {
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
    pitch = std::copysign(M_PI / 2.0, sinp);
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

PJ::sdk::ValueRef variantToValueRef(const RosMsgParser::Variant& variant) {
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

double valueRefAsDouble(const PJ::sdk::ValueRef& v) {
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

// Try to parse a string as a double. Returns (value, true) on success.
std::pair<double, bool> tryParseDouble(const std::string& s) {
  if (s.empty()) return {0.0, false};
  char* end = nullptr;
  double val = std::strtod(s.c_str(), &end);
  if (end != s.c_str() && *end == '\0') return {val, true};
  return {0.0, false};
}

// Strip "/names" or "/values" suffix from a topic string for PAL statistics key.
std::string palStatisticsKey(const std::string& topic) {
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
  PJ::Status bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) override {
    std::string definition(reinterpret_cast<const char*>(schema.data()), schema.size());

    // Normalise ROS 2 type names: "pkg/msg/Type" -> "pkg/Type"
    type_name_ = std::string(type_name);
    std::string msg_type = type_name_;
    if (auto pos = msg_type.find("/msg/"); pos != std::string::npos) {
      msg_type.erase(pos, 4);
    }

    try {
      parser_.emplace("", RosMsgParser::ROSType(msg_type), definition);
      auto policy = discard_large_arrays_ ? RosMsgParser::Parser::DISCARD_LARGE_ARRAYS
                                          : RosMsgParser::Parser::KEEP_LARGE_ARRAYS;
      parser_->setMaxArrayPolicy(policy, max_array_size_);
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("failed to parse ROS schema: ") + e.what());
    }

    detectSpecialization(msg_type);
    detectSchemaFeatures();
    ensureDeserializer();
    return PJ::okStatus();
  }

  std::string saveConfig() const override {
    nlohmann::json cfg;
    cfg["max_array_size"] = max_array_size_;
    cfg["discard_large_arrays"] = discard_large_arrays_;
    cfg["use_embedded_timestamp"] = use_embedded_timestamp_;
    cfg["serialization"] = use_ros1_ ? "ros1" : "cdr";
    if (!topic_name_.empty()) cfg["topic_name"] = topic_name_;
    return cfg.dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) return PJ::okStatus();

    max_array_size_ = static_cast<size_t>(cfg.value("max_array_size", 500));
    discard_large_arrays_ = cfg.value("discard_large_arrays", false);
    use_embedded_timestamp_ = cfg.value("use_embedded_timestamp", false);
    topic_name_ = cfg.value("topic_name", std::string{});

    bool new_ros1 = (cfg.value("serialization", "cdr") == "ros1");
    if (new_ros1 != use_ros1_) {
      use_ros1_ = new_ros1;
      deserializer_.reset();  // force re-creation
    }

    if (parser_.has_value()) {
      auto policy = discard_large_arrays_ ? RosMsgParser::Parser::DISCARD_LARGE_ARRAYS
                                           : RosMsgParser::Parser::KEEP_LARGE_ARRAYS;
      parser_->setMaxArrayPolicy(policy, max_array_size_);
    }
    ensureDeserializer();
    return PJ::okStatus();
  }

  PJ::Status parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) override {
    if (!writeHostBound()) return PJ::unexpected(std::string("write host not bound"));
    if (!parser_.has_value()) return PJ::unexpected(std::string("no schema bound"));
    ensureDeserializer();

    owned_fields_.clear();
    string_storage_.clear();
    named_fields_.clear();
    current_timestamp_ = timestamp_ns;

    if (specialization_ != Specialization::kNone) {
      deserializer_->init(
          RosMsgParser::Span<const uint8_t>(payload.data(), payload.size()));

      switch (specialization_) {
        case Specialization::kEmpty:
          handleEmpty();
          break;
        case Specialization::kPose:
          handlePose();
          break;
        case Specialization::kPoseStamped:
          handlePoseStamped();
          break;
        case Specialization::kTransform:
          handleTransform();
          break;
        case Specialization::kTransformStamped:
          handleTransformStamped();
          break;
        case Specialization::kImu:
          handleImu();
          break;
        case Specialization::kOdometry:
          handleOdometry();
          break;
        case Specialization::kJointState:
          handleJointState();
          break;
        case Specialization::kDiagnosticArray:
          handleDiagnosticArray();
          break;
        case Specialization::kTFMessage:
          handleTFMessage();
          break;
        case Specialization::kDataTamerSchemas:
          handleDataTamerSchemas();
          break;
        case Specialization::kDataTamerSnapshot:
          handleDataTamerSnapshot();
          break;
        case Specialization::kPalStatisticsNames:
          handlePalStatisticsNames();
          break;
        case Specialization::kPalStatisticsValues:
          handlePalStatisticsValues();
          break;
        case Specialization::kTSLDefinition:
          handleTSLDefinition();
          break;
        case Specialization::kTSLValues:
          handleTSLValues();
          break;
        default:
          break;
      }
    } else {
      flattenGeneric(payload);
    }

    if (!owned_fields_.empty()) {
      return emitRecord(current_timestamp_);
    }
    return PJ::okStatus();
  }

 private:
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

  // -----------------------------------------------------------------------
  // Setup helpers
  // -----------------------------------------------------------------------

  void ensureDeserializer() {
    bool need_create = !deserializer_ || (use_ros1_ == deserializer_->isROS2());
    if (need_create) {
      if (use_ros1_) {
        deserializer_ = std::make_unique<RosMsgParser::ROS_Deserializer>();
      } else {
        deserializer_ = std::make_unique<RosMsgParser::NanoCDR_Deserializer>();
      }
    }
  }

  void detectSpecialization(const std::string& msg_type) {
    static const std::unordered_map<std::string, Specialization> kMap = {
        {"std_msgs/Empty", Specialization::kEmpty},
        {"sensor_msgs/JointState", Specialization::kJointState},
        {"diagnostic_msgs/DiagnosticArray", Specialization::kDiagnosticArray},
        {"tf2_msgs/TFMessage", Specialization::kTFMessage},
        {"data_tamer_msgs/Schemas", Specialization::kDataTamerSchemas},
        {"data_tamer_msgs/Snapshot", Specialization::kDataTamerSnapshot},
        {"sensor_msgs/Imu", Specialization::kImu},
        {"geometry_msgs/Pose", Specialization::kPose},
        {"geometry_msgs/PoseStamped", Specialization::kPoseStamped},
        {"nav_msgs/Odometry", Specialization::kOdometry},
        {"geometry_msgs/Transform", Specialization::kTransform},
        {"geometry_msgs/TransformStamped", Specialization::kTransformStamped},
        {"pal_statistics_msgs/StatisticsNames", Specialization::kPalStatisticsNames},
        {"pal_statistics_msgs/StatisticsValues", Specialization::kPalStatisticsValues},
        {"plotjuggler_msgs/StatisticsNames", Specialization::kPalStatisticsNames},
        {"plotjuggler_msgs/StatisticsValues", Specialization::kPalStatisticsValues},
        {"tsl_msgs/TSLDefinition", Specialization::kTSLDefinition},
        {"tsl_msgs/TSLValues", Specialization::kTSLValues},
    };
    auto it = kMap.find(msg_type);
    specialization_ = (it != kMap.end()) ? it->second : Specialization::kNone;
  }

  void detectSchemaFeatures() {
    const auto& schema = parser_->getSchema();
    const auto& root_fields = schema->root_msg->fields();

    has_header_ =
        !root_fields.empty() && root_fields.front().type().baseName() == "std_msgs/Header";

    quaternion_prefixes_.clear();
    findQuaternionPrefixes(schema->root_msg.get(), "", schema->msg_library);
  }

  void findQuaternionPrefixes(const RosMsgParser::ROSMessage* msg, const std::string& prefix,
                              const RosMsgParser::RosMessageLibrary& lib) {
    for (const auto& field : msg->fields()) {
      if (field.isConstant()) continue;

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

  // -----------------------------------------------------------------------
  // Field accumulation helpers
  // -----------------------------------------------------------------------

  void addField(const std::string& name, double value) {
    owned_fields_.push_back({name, PJ::sdk::ValueRef{value}});
  }

  void addField(const std::string& name, PJ::sdk::ValueRef value) {
    owned_fields_.push_back({name, value});
  }

  void addStringField(const std::string& name, const std::string& value) {
    string_storage_.push_back(value);
    owned_fields_.push_back({name, PJ::sdk::ValueRef{std::string_view(string_storage_.back())}});
  }

  // -----------------------------------------------------------------------
  // Emit record
  // -----------------------------------------------------------------------

  PJ::Status emitRecord(PJ::Timestamp ts) {
    named_fields_.clear();
    named_fields_.reserve(owned_fields_.size());
    for (const auto& f : owned_fields_) {
      named_fields_.push_back({.name = f.name, .value = f.value});
    }
    return writeHost().appendRecord(
        ts, PJ::Span<const PJ::sdk::NamedFieldValue>(named_fields_.data(), named_fields_.size()));
  }

  // -----------------------------------------------------------------------
  // Header helpers
  // -----------------------------------------------------------------------

  struct HeaderData {
    uint32_t seq = 0;
    uint32_t sec = 0;
    uint32_t nsec = 0;
    std::string frame_id;
  };

  HeaderData readHeader() {
    HeaderData h;
    if (!deserializer_->isROS2()) {
      h.seq = deserializer_->deserializeUInt32();
    }
    h.sec = deserializer_->deserializeUInt32();
    h.nsec = deserializer_->deserializeUInt32();

    if (use_embedded_timestamp_) {
      int64_t ts_ns =
          static_cast<int64_t>(h.sec) * 1000000000LL + static_cast<int64_t>(h.nsec);
      if (ts_ns > 0) current_timestamp_ = ts_ns;
    }

    deserializer_->deserializeString(h.frame_id);
    return h;
  }

  void emitHeader(const HeaderData& h) {
    double stamp = static_cast<double>(h.sec) + static_cast<double>(h.nsec) * 1e-9;
    addField("/header/stamp", stamp);
    addStringField("/header/frame_id", h.frame_id);
    if (!deserializer_->isROS2()) {
      addField("/header/seq", static_cast<double>(h.seq));
    }
  }

  // -----------------------------------------------------------------------
  // Composition parse helpers (read from deserializer, add to owned_fields_)
  // -----------------------------------------------------------------------

  void parseVector3(const std::string& prefix) {
    double x = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    double y = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    double z = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    addField(prefix + "/x", x);
    addField(prefix + "/y", y);
    addField(prefix + "/z", z);
  }

  void parsePoint(const std::string& prefix) {
    parseVector3(prefix);  // same wire format: 3 × float64
  }

  void parseQuaternion(const std::string& prefix) {
    double x = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    double y = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    double z = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    double w = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    addField(prefix + "/x", x);
    addField(prefix + "/y", y);
    addField(prefix + "/z", z);
    addField(prefix + "/w", w);

    auto rpy = quaternionToRPY(x, y, z, w);
    addField(prefix + "/roll", rpy.roll);
    addField(prefix + "/pitch", rpy.pitch);
    addField(prefix + "/yaw", rpy.yaw);
  }

  template <size_t N>
  void parseCovariance(const std::string& prefix) {
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

  void parseTwist(const std::string& prefix) {
    parseVector3(prefix + "/linear");
    parseVector3(prefix + "/angular");
  }

  void parsePose(const std::string& prefix) {
    parseVector3(prefix + "/position");
    parseQuaternion(prefix + "/orientation");
  }

  void parseTransform(const std::string& prefix) {
    parsePoint(prefix + "/translation");
    parseQuaternion(prefix + "/rotation");
  }

  void parsePoseWithCovariance(const std::string& prefix) {
    parsePose(prefix + "/pose");
    parseCovariance<6>(prefix + "/covariance");
  }

  void parseTwistWithCovariance(const std::string& prefix) {
    parseTwist(prefix + "/twist");
    parseCovariance<6>(prefix + "/covariance");
  }

  // -----------------------------------------------------------------------
  // Top-level specialized handlers
  // -----------------------------------------------------------------------

  void handleEmpty() {
    addField("/value", 0.0);
  }

  void handlePose() {
    parsePose("");
  }

  void handlePoseStamped() {
    auto h = readHeader();
    emitHeader(h);
    parsePose("/pose");
  }

  void handleTransform() {
    parseTransform("");
  }

  void handleTransformStamped() {
    auto h = readHeader();
    emitHeader(h);
    std::string child_frame_id;
    deserializer_->deserializeString(child_frame_id);
    addStringField("/child_frame_id", child_frame_id);
    parseTransform("/transform");
  }

  void handleImu() {
    auto h = readHeader();
    emitHeader(h);
    parseQuaternion("/orientation");
    parseCovariance<3>("/orientation_covariance");
    parseVector3("/angular_velocity");
    parseCovariance<3>("/angular_velocity_covariance");
    parseVector3("/linear_acceleration");
    parseCovariance<3>("/linear_acceleration_covariance");
  }

  void handleOdometry() {
    auto h = readHeader();
    emitHeader(h);
    std::string child_frame_id;
    deserializer_->deserializeString(child_frame_id);
    addStringField("/child_frame_id", child_frame_id);
    parsePoseWithCovariance("/pose");
    parseTwistWithCovariance("/twist");
  }

  void handleJointState() {
    auto h = readHeader();
    emitHeader(h);

    size_t name_count = deserializer_->deserializeUInt32();
    std::vector<std::string> names(name_count);
    for (auto& name : names) {
      deserializer_->deserializeString(name);
    }

    size_t pos_count = deserializer_->deserializeUInt32();
    std::vector<double> positions(pos_count);
    for (auto& p : positions) {
      p = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    }

    size_t vel_count = deserializer_->deserializeUInt32();
    std::vector<double> velocities(vel_count);
    for (auto& v : velocities) {
      v = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    }

    size_t eff_count = deserializer_->deserializeUInt32();
    std::vector<double> efforts(eff_count);
    for (auto& e : efforts) {
      e = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    }

    for (size_t i = 0; i < std::min(name_count, pos_count); i++) {
      addField("/" + names[i] + "/position", positions[i]);
    }
    for (size_t i = 0; i < std::min(name_count, vel_count); i++) {
      addField("/" + names[i] + "/velocity", velocities[i]);
    }
    for (size_t i = 0; i < std::min(name_count, eff_count); i++) {
      addField("/" + names[i] + "/effort", efforts[i]);
    }
  }

  void handleDiagnosticArray() {
    auto h = readHeader();
    emitHeader(h);

    size_t status_count = deserializer_->deserializeUInt32();
    for (size_t st = 0; st < status_count; st++) {
      uint8_t level =
          deserializer_->deserialize(RosMsgParser::BYTE).convert<uint8_t>();
      std::string name;
      deserializer_->deserializeString(name);
      std::string message;
      deserializer_->deserializeString(message);
      std::string hardware_id;
      deserializer_->deserializeString(hardware_id);

      // Build prefix: /{hardware_id}/{name} or /{name}
      std::string status_prefix;
      if (hardware_id.empty()) {
        status_prefix = "/" + name;
      } else {
        status_prefix = "/" + hardware_id + "/" + name;
      }

      addField(status_prefix + "/level", static_cast<double>(level));
      addStringField(status_prefix + "/message", message);

      size_t kv_count = deserializer_->deserializeUInt32();
      for (size_t kv = 0; kv < kv_count; kv++) {
        std::string key;
        deserializer_->deserializeString(key);
        std::string value_str;
        deserializer_->deserializeString(value_str);

        auto [dval, ok] = tryParseDouble(value_str);
        if (ok) {
          addField(status_prefix + "/" + key, dval);
        } else {
          addStringField(status_prefix + "/" + key, value_str);
        }
      }
    }
  }

  void handleTFMessage() {
    size_t transform_count = deserializer_->deserializeUInt32();
    for (size_t i = 0; i < transform_count; i++) {
      auto h = readHeader();
      std::string child_frame_id;
      deserializer_->deserializeString(child_frame_id);

      std::string prefix;
      if (h.frame_id.empty()) {
        prefix = "/" + child_frame_id;
      } else {
        prefix = "/" + h.frame_id + "/" + child_frame_id;
      }
      parseTransform(prefix);
    }
  }

  // -----------------------------------------------------------------------
  // Cross-message state handlers
  // -----------------------------------------------------------------------

  void handleDataTamerSchemas() {
    size_t count = deserializer_->deserializeUInt32();
    for (size_t i = 0; i < count; i++) {
      // Wire hash (for lookup by Snapshot messages).
      auto wire_hash =
          deserializer_->deserialize(RosMsgParser::UINT64).convert<uint64_t>();
      std::string channel_name;
      deserializer_->deserializeString(channel_name);
      std::string schema_text;
      deserializer_->deserializeString(schema_text);

      auto schema = DataTamerParser::BuilSchemaFromText(schema_text);
      schema.hash = wire_hash;
      schema.channel_name = channel_name;
      g_data_tamer_schemas.insert({wire_hash, schema});
    }
    // No fields to emit for schemas message itself.
  }

  void handleDataTamerSnapshot() {
    uint64_t timestamp =
        deserializer_->deserialize(RosMsgParser::UINT64).convert<uint64_t>();
    uint64_t schema_hash =
        deserializer_->deserialize(RosMsgParser::UINT64).convert<uint64_t>();

    auto active_mask = deserializer_->deserializeByteSequence();
    auto payload = deserializer_->deserializeByteSequence();

    auto it = g_data_tamer_schemas.find(schema_hash);
    if (it == g_data_tamer_schemas.end()) return;

    const auto& schema = it->second;
    DataTamerParser::SnapshotView snapshot;
    snapshot.schema_hash = schema_hash;
    snapshot.timestamp = timestamp;
    snapshot.active_mask = {active_mask.data(), active_mask.size()};
    snapshot.payload = {payload.data(), payload.size()};

    // Use the snapshot's own timestamp (nanoseconds).
    current_timestamp_ = static_cast<int64_t>(timestamp);

    const auto to_double = [](const auto& value) { return static_cast<double>(value); };

    DataTamerParser::ParseSnapshot(
        schema, snapshot,
        [&](const std::string& field_name, const DataTamerParser::VarNumber& value) {
          addField("/" + schema.channel_name + "/" + field_name,
                   std::visit(to_double, value));
        });
  }

  void handlePalStatisticsNames() {
    auto h = readHeader();
    // Don't emit header fields for this meta-message.

    size_t count = deserializer_->deserializeUInt32();
    std::vector<std::string> names(count);
    for (auto& name : names) {
      deserializer_->deserializeString(name);
    }
    uint32_t version = deserializer_->deserializeUInt32();

    std::string key = palStatisticsKey(topic_name_);
    g_pal_statistics_names[key][version] = std::move(names);
    // No fields to emit.
  }

  void handlePalStatisticsValues() {
    auto h = readHeader();
    emitHeader(h);

    size_t count = deserializer_->deserializeUInt32();
    std::vector<double> values(count);
    for (auto& v : values) {
      v = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
    }
    uint32_t version = deserializer_->deserializeUInt32();

    std::string key = palStatisticsKey(topic_name_);
    auto topic_it = g_pal_statistics_names.find(key);
    if (topic_it == g_pal_statistics_names.end()) return;

    auto ver_it = topic_it->second.find(version);
    if (ver_it == topic_it->second.end()) return;

    const auto& names = ver_it->second;
    size_t n = std::min(names.size(), values.size());
    // Clear the header fields already emitted — for PAL values we emit named fields only.
    owned_fields_.clear();
    string_storage_.clear();
    for (size_t i = 0; i < n; i++) {
      addField("/" + names[i], values[i]);
    }
  }

  void handleTSLDefinition() {
    (void)deserializer_->deserializeUInt32();  // stamp sec
    (void)deserializer_->deserializeUInt32();  // stamp nsec
    uint64_t hash =
        deserializer_->deserialize(RosMsgParser::UINT64).extract<uint64_t>();

    if (g_tsl_definitions.count(hash) != 0) return;  // already known

    std::vector<std::string> definition;
    for (auto type : kTslTypeOrder) {
      (void)type;
      size_t num = deserializer_->deserializeUInt32();
      size_t base = definition.size();
      definition.resize(base + num);
      for (size_t i = base; i < definition.size(); i++) {
        deserializer_->deserializeString(definition[i]);
      }
    }

    g_tsl_definitions[hash] = std::move(definition);

    // Flush buffered values.
    auto& queue = g_tsl_values_buffer[hash];
    while (!queue.empty()) {
      auto& [buf_ts, buf_values] = queue.front();
      owned_fields_.clear();
      string_storage_.clear();
      const auto& def = g_tsl_definitions[hash];
      size_t n = std::min(def.size(), buf_values.size());
      for (size_t i = 0; i < n; i++) {
        addField("/" + def[i], buf_values[i]);
      }
      if (!owned_fields_.empty()) {
        // Ignore error from buffered flush — best effort.
        (void)emitRecord(buf_ts);
      }
      queue.pop();
    }
    owned_fields_.clear();
    string_storage_.clear();
    // No fields to emit for the definition itself.
  }

  void handleTSLValues() {
    (void)deserializer_->deserializeUInt32();  // stamp sec
    (void)deserializer_->deserializeUInt32();  // stamp nsec
    uint64_t hash =
        deserializer_->deserialize(RosMsgParser::UINT64).extract<uint64_t>();

    std::vector<double> values;
    for (auto type_id : kTslTypeOrder) {
      size_t num = deserializer_->deserializeUInt32();
      size_t base = values.size();
      values.resize(base + num);
      for (size_t i = base; i < values.size(); i++) {
        values[i] = deserializer_->deserialize(type_id).convert<double>();
      }
    }

    if (g_tsl_definitions.count(hash) == 0) {
      g_tsl_values_buffer[hash].push({current_timestamp_, std::move(values)});
      return;
    }

    const auto& def = g_tsl_definitions[hash];
    size_t n = std::min(def.size(), values.size());
    for (size_t i = 0; i < n; i++) {
      addField("/" + def[i], values[i]);
    }
  }

  // -----------------------------------------------------------------------
  // Generic path
  // -----------------------------------------------------------------------

  void flattenGeneric(PJ::Span<const uint8_t> payload) {
    try {
      parser_->deserialize(
          RosMsgParser::Span<const uint8_t>(payload.data(), payload.size()),
          &flat_msg_, deserializer_.get());
    } catch (const std::exception& e) {
      // Store error but still try to emit what we have.
      setLastError(std::string("CDR deserialization failed: ") + e.what());
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
        string_storage_.push_back(variant.extract<std::string>());
        owned_fields_.push_back(
            {field_name, PJ::sdk::ValueRef{std::string_view(string_storage_.back())}});
      } else {
        owned_fields_.push_back({field_name, variantToValueRef(variant)});
      }
    }

    addQuaternionRPY();
  }

  void addQuaternionRPY() {
    if (quaternion_prefixes_.empty()) return;

    // Build name → index map for O(1) lookup.
    std::unordered_map<std::string, size_t> idx;
    const size_t n = owned_fields_.size();
    for (size_t i = 0; i < n; i++) {
      idx.emplace(owned_fields_[i].name, i);
    }

    for (const auto& prefix : quaternion_prefixes_) {
      auto find_val = [&](const std::string& suffix) -> double {
        auto it = idx.find(prefix + suffix);
        if (it == idx.end()) return 0.0;
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
};

}  // namespace

PJ_MESSAGE_PARSER_PLUGIN(RosParser,
                         R"({"name":"ROS CDR Parser","version":"2.0.0","encoding":"cdr"})")
