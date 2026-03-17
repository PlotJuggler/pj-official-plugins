#include "ros_parser_internal.hpp"

namespace ros_parser_detail {

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

PJ::Status RosParser::bindSchema(std::string_view type_name, PJ::Span<const uint8_t> schema) {
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

std::string RosParser::saveConfig() const {
  nlohmann::json cfg;
  cfg["max_array_size"] = max_array_size_;
  cfg["discard_large_arrays"] = discard_large_arrays_;
  cfg["use_embedded_timestamp"] = use_embedded_timestamp_;
  cfg["serialization"] = use_ros1_ ? "ros1" : "cdr";
  if (!topic_name_.empty()) cfg["topic_name"] = topic_name_;
  return cfg.dump();
}

PJ::Status RosParser::loadConfig(std::string_view config_json) {
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

PJ::Status RosParser::parse(PJ::Timestamp timestamp_ns, PJ::Span<const uint8_t> payload) {
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

void RosParser::detectSpecialization(const std::string& msg_type) {
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

void RosParser::detectSchemaFeatures() {
  const auto& schema = parser_->getSchema();
  const auto& root_fields = schema->root_msg->fields();

  has_header_ =
      !root_fields.empty() && root_fields.front().type().baseName() == "std_msgs/Header";

  quaternion_prefixes_.clear();
  findQuaternionPrefixes(schema->root_msg.get(), "", schema->msg_library);
}

void RosParser::findQuaternionPrefixes(const RosMsgParser::ROSMessage* msg, const std::string& prefix,
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
    int64_t ts_ns =
        static_cast<int64_t>(h.sec) * 1000000000LL + static_cast<int64_t>(h.nsec);
    if (ts_ns > 0) current_timestamp_ = ts_ns;
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

void RosParser::addQuaternionRPY() {
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

}  // namespace ros_parser_detail

PJ_MESSAGE_PARSER_PLUGIN(ros_parser_detail::RosParser,
                         R"({"name":"ROS CDR Parser","version":"2.0.0",)"
                         R"("encoding":["ros2msg","ros1msg","cdr"]})")
