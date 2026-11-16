#include <data_tamer_v2/data_tamer_parser.hpp>
#include <queue>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "ros_parser_internal.hpp"

namespace ros_parser_detail {

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
thread_local std::unordered_map<uint64_t, std::queue<std::tuple<int64_t, std::vector<double>>>> g_tsl_values_buffer;

// TSL type order (matches old parser)
constexpr std::array<RosMsgParser::BuiltinType, 11> kTslTypeOrder = {
    RosMsgParser::BOOL,   RosMsgParser::INT8,    RosMsgParser::UINT8,   RosMsgParser::INT16,
    RosMsgParser::UINT16, RosMsgParser::INT32,   RosMsgParser::UINT32,  RosMsgParser::INT64,
    RosMsgParser::UINT64, RosMsgParser::FLOAT32, RosMsgParser::FLOAT64,
};

}  // namespace

// ---------------------------------------------------------------------------
// Composition parse helpers
// ---------------------------------------------------------------------------

void RosParser::addFieldAt(std::string_view segment, double value) {
  PathSegment field(path_, segment);
  addField(path_, value);
}

void RosParser::parseVector3(std::string_view segment) {
  double x = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  double y = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  double z = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  PathSegment vector(path_, segment);
  addFieldAt("/x", x);
  addFieldAt("/y", y);
  addFieldAt("/z", z);
}

void RosParser::parsePoint(std::string_view segment) {
  parseVector3(segment);  // same wire format: 3 × float64
}

void RosParser::parseQuaternion(std::string_view segment) {
  double x = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  double y = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  double z = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  double w = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  PathSegment quaternion(path_, segment);
  addFieldAt("/x", x);
  addFieldAt("/y", y);
  addFieldAt("/z", z);
  addFieldAt("/w", w);

  auto rpy = quaternionToRPY(x, y, z, w);
  addFieldAt("/roll", rpy.roll);
  addFieldAt("/pitch", rpy.pitch);
  addFieldAt("/yaw", rpy.yaw);
}

void RosParser::parseTwist(std::string_view segment) {
  PathSegment twist(path_, segment);
  parseVector3("/linear");
  parseVector3("/angular");
}

void RosParser::parsePose(std::string_view segment) {
  emitPose(segment, readPose());
}

void RosParser::emitPose(std::string_view segment, const PJ::sdk::Pose& pose) {
  PathSegment prefix(path_, segment);
  addFieldAt("/position/x", pose.position.x);
  addFieldAt("/position/y", pose.position.y);
  addFieldAt("/position/z", pose.position.z);
  const auto& q = pose.orientation;
  addFieldAt("/orientation/x", q.x);
  addFieldAt("/orientation/y", q.y);
  addFieldAt("/orientation/z", q.z);
  addFieldAt("/orientation/w", q.w);
  const auto rpy = quaternionToRPY(q.x, q.y, q.z, q.w);
  addFieldAt("/orientation/roll", rpy.roll);
  addFieldAt("/orientation/pitch", rpy.pitch);
  addFieldAt("/orientation/yaw", rpy.yaw);
}

void RosParser::parseTransform(std::string_view segment) {
  PathSegment transform(path_, segment);
  parsePoint("/translation");
  parseQuaternion("/rotation");
}

void RosParser::parsePoseWithCovariance(std::string_view segment) {
  PathSegment prefix(path_, segment);
  parsePose("/pose");
  parseCovariance<6>("/covariance");
}

void RosParser::parseTwistWithCovariance(std::string_view segment) {
  PathSegment prefix(path_, segment);
  parseTwist("/twist");
  parseCovariance<6>("/covariance");
}

size_t RosParser::readArrayCount(size_t min_element_wire_size) {
  const size_t count = deserializer_->deserializeUInt32();
  if (count > deserializer_->bytesLeft() / min_element_wire_size) {
    throw std::runtime_error("array count exceeds the message payload (truncated or corrupt message)");
  }
  return count;
}

// ---------------------------------------------------------------------------
// Top-level specialized handlers
// ---------------------------------------------------------------------------

void RosParser::handleEmpty() {
  addField("/value", 0.0);
}

void RosParser::handlePose() {
  path_.clear();
  parsePose("");
}

void RosParser::handlePoseStamped() {
  const auto& h = readHeader();
  emitHeader(h);
  path_.clear();
  parsePose("/pose");
}

void RosParser::handleTransform() {
  path_.clear();
  parseTransform("");
}

void RosParser::handleTransformStamped() {
  const auto& h = readHeader();
  emitHeader(h);
  deserializer_->deserializeString(child_frame_id_);
  addStringField("/child_frame_id", child_frame_id_);
  path_.clear();
  parseTransform("/transform");
}

void RosParser::handleImu() {
  const auto& h = readHeader();
  emitHeader(h);
  path_.clear();
  parseQuaternion("/orientation");
  parseCovariance<3>("/orientation_covariance");
  parseVector3("/angular_velocity");
  parseCovariance<3>("/angular_velocity_covariance");
  parseVector3("/linear_acceleration");
  parseCovariance<3>("/linear_acceleration_covariance");
}

void RosParser::handleOdometry() {
  const auto& h = readHeader();
  emitHeader(h);
  deserializer_->deserializeString(child_frame_id_);
  addStringField("/child_frame_id", child_frame_id_);
  path_.clear();
  parsePoseWithCovariance("/pose");
  parseTwistWithCovariance("/twist");
}

void RosParser::handleJointState() {
  const auto& h = readHeader();
  emitHeader(h);

  // Member scratch: the vectors and the name strings keep their capacity.
  auto& names = joint_names_;
  auto& positions = joint_positions_;
  auto& velocities = joint_velocities_;
  auto& efforts = joint_efforts_;

  const size_t name_count = readArrayCount(sizeof(uint32_t));  // a string is at least its length word
  names.resize(name_count);
  for (auto& name : names) {
    deserializer_->deserializeString(name);
  }

  const size_t pos_count = readArrayCount(sizeof(double));
  positions.resize(pos_count);
  for (auto& p : positions) {
    p = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  }

  const size_t vel_count = readArrayCount(sizeof(double));
  velocities.resize(vel_count);
  for (auto& v : velocities) {
    v = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  }

  const size_t eff_count = readArrayCount(sizeof(double));
  efforts.resize(eff_count);
  for (auto& e : efforts) {
    e = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  }

  path_.assign("/");
  const auto add_joint_fields = [this, &names](
                                    size_t count, const std::vector<double>& values, std::string_view suffix) {
    for (size_t i = 0; i < count; i++) {
      PathSegment joint(path_, names[i]);
      addFieldAt(suffix, values[i]);
    }
  };
  add_joint_fields(std::min(name_count, pos_count), positions, "/position");
  add_joint_fields(std::min(name_count, vel_count), velocities, "/velocity");
  add_joint_fields(std::min(name_count, eff_count), efforts, "/effort");
}

void RosParser::handleDiagnosticArray() {
  const auto& h = readHeader();
  emitHeader(h);

  // Field names must be unique within one message, but real diagnostics repeat
  // keys (e.g. one "Interface Name" per network interface) and repeat status
  // names. Suffix repeats as name[1], name[2], ... so nothing is dropped.
  // The counts live in a member map stamped with the message they belong to, so
  // a recurring name costs a lookup, not an allocation.
  if (diag_seen_.size() > 4096) {
    diag_seen_.clear();  // bounds the map when keys are not a fixed set
  }
  ++diag_message_;
  const auto unique = [this](std::string& name) {
    auto& [message, count] = diag_seen_.try_emplace(name, 0, 0).first->second;
    if (message != diag_message_) {
      message = diag_message_;
      count = 0;
    }
    if (count > 0) {
      name += '[';
      name += std::to_string(count);
      name += ']';
    }
    ++count;
  };

  size_t status_count = deserializer_->deserializeUInt32();
  for (size_t st = 0; st < status_count; st++) {
    // diagnostic_msgs/DiagnosticStatus: level, name, message and hardware_id in the
    // order the bound schema declares (standard: level first), then values[]. The
    // OK/WARN/ERROR/STALE byte constants are not serialized.
    uint8_t level = 0;
    for (const DiagField field : diag_order_) {
      switch (field) {
        case DiagField::kLevel:
          level = deserializer_->deserialize(RosMsgParser::BYTE).convert<uint8_t>();
          break;
        case DiagField::kName:
          deserializer_->deserializeString(diag_name_);
          break;
        case DiagField::kMessage:
          deserializer_->deserializeString(diag_message_text_);
          break;
        case DiagField::kHardwareId:
          deserializer_->deserializeString(diag_hardware_id_);
          break;
      }
    }

    // Prefix: /{hardware_id}/{name} or /{name}
    path_.assign("/");
    if (!diag_hardware_id_.empty()) {
      path_ += diag_hardware_id_;
      path_ += '/';
    }
    path_ += diag_name_;
    const size_t prefix_size = path_.size();

    path_ += "/level";
    unique(path_);
    addField(path_, static_cast<double>(level));
    path_.resize(prefix_size);
    path_ += "/message";
    unique(path_);
    addStringField(path_, diag_message_text_);
    path_.resize(prefix_size);

    size_t kv_count = deserializer_->deserializeUInt32();
    for (size_t kv = 0; kv < kv_count; kv++) {
      deserializer_->deserializeString(diag_key_);
      deserializer_->deserializeString(diag_value_);

      if (diag_key_.empty()) {
        continue;  // Padding entries some publishers emit; they carry nothing.
      }
      path_ += '/';
      path_ += diag_key_;
      unique(path_);
      auto [dval, ok] = tryParseDouble(diag_value_);
      if (ok) {
        addField(path_, dval);
      } else {
        addStringField(path_, diag_value_);
      }
      path_.resize(prefix_size);
    }
  }
}

void RosParser::handleTFMessage() {
  size_t transform_count = deserializer_->deserializeUInt32();
  for (size_t i = 0; i < transform_count; i++) {
    const auto& h = readHeader();
    deserializer_->deserializeString(child_frame_id_);

    // Prefix: /{frame_id}/{child_frame_id}, or /{child_frame_id} without a frame_id.
    path_.assign("/");
    if (!h.frame_id.empty()) {
      path_ += h.frame_id;
      path_ += '/';
    }
    path_ += child_frame_id_;
    // Per-transform header (the batch has no single message-level Header). The
    // parent frame_id is already the path prefix, so only the stamp (seconds) —
    // and seq on ROS1 — are emitted, matching the /header/stamp series elsewhere.
    addFieldAt("/header/stamp", static_cast<double>(h.sec) + static_cast<double>(h.nsec) * 1e-9);
    if (!deserializer_->isROS2()) {
      addFieldAt("/header/seq", static_cast<double>(h.seq));
    }
    parseTransform("");
  }
}

// ---------------------------------------------------------------------------
// Cross-message state handlers
// ---------------------------------------------------------------------------

void RosParser::handleDataTamerSchemas() {
  size_t count = deserializer_->deserializeUInt32();
  for (size_t i = 0; i < count; i++) {
    auto wire_hash = deserializer_->deserialize(RosMsgParser::UINT64).convert<uint64_t>();
    std::string channel_name;
    deserializer_->deserializeString(channel_name);
    std::string schema_text;
    deserializer_->deserializeString(schema_text);

    auto schema = DataTamerParser::BuildSchemaFromText(schema_text);
    schema.hash = wire_hash;
    schema.channel_name = channel_name;
    g_data_tamer_schemas.insert({wire_hash, schema});
  }
  // No fields to emit for schemas message itself.
}

void RosParser::handleDataTamerSnapshot() {
  uint64_t timestamp = deserializer_->deserialize(RosMsgParser::UINT64).convert<uint64_t>();
  uint64_t schema_hash = deserializer_->deserialize(RosMsgParser::UINT64).convert<uint64_t>();

  auto active_mask = readByteSequence();
  auto payload = readByteSequence();

  auto it = g_data_tamer_schemas.find(schema_hash);
  if (it == g_data_tamer_schemas.end()) {
    return;
  }

  const auto& schema = it->second;
  DataTamerParser::SnapshotView snapshot;
  snapshot.schema_hash = schema_hash;
  snapshot.timestamp = timestamp;
  snapshot.active_mask = {active_mask.data(), active_mask.size()};
  snapshot.payload = {payload.data(), payload.size()};

  // Use the snapshot's own timestamp (nanoseconds).
  current_timestamp_ = static_cast<int64_t>(timestamp);

  const auto to_double = [](const auto& value) { return static_cast<double>(value); };

  const std::string prefix = "/" + schema.channel_name + "/";
  const bool parsed = DataTamerParser::ParseSnapshot(
      schema, snapshot,
      [&](const std::string& field_name, const DataTamerParser::VarNumber& value) {
        addField(prefix + field_name, std::visit(to_double, value));
      },
      [&](const std::string& field_name, std::string_view text) {
        addStringField(prefix + field_name, std::string(text));
      });
  if (!parsed) {
    throw std::runtime_error("DataTamer snapshot payload does not match the schema of channel " + schema.channel_name);
  }
}

void RosParser::handlePalStatisticsNames() {
  readHeader();  // Don't emit header fields for this meta-message.

  size_t count = deserializer_->deserializeUInt32();
  std::vector<std::string> names(count);
  for (auto& name : names) {
    deserializer_->deserializeString(name);
  }
  uint32_t version = deserializer_->deserializeUInt32();

  g_pal_statistics_names[pal_statistics_key_][version] = std::move(names);
  // No fields to emit.
}

void RosParser::handlePalStatisticsValues() {
  const auto& h = readHeader();
  emitHeader(h);

  auto& values = statistics_values_;
  values.resize(readArrayCount(sizeof(double)));
  for (auto& v : values) {
    v = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  }
  uint32_t version = deserializer_->deserializeUInt32();

  auto topic_it = g_pal_statistics_names.find(pal_statistics_key_);
  if (topic_it == g_pal_statistics_names.end()) {
    return;
  }

  auto ver_it = topic_it->second.find(version);
  if (ver_it == topic_it->second.end()) {
    return;
  }

  const auto& names = ver_it->second;
  size_t n = std::min(names.size(), values.size());
  // Clear the header fields already emitted — for PAL values we emit named fields only.
  fields_.clear();
  path_.assign("/");
  for (size_t i = 0; i < n; i++) {
    addFieldAt(names[i], values[i]);
  }
}

void RosParser::handleTSLDefinition() {
  (void)deserializer_->deserializeUInt32();  // stamp sec
  (void)deserializer_->deserializeUInt32();  // stamp nsec
  uint64_t hash = deserializer_->deserialize(RosMsgParser::UINT64).extract<uint64_t>();

  if (g_tsl_definitions.count(hash) != 0) {
    return;  // already known
  }

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
  path_.assign("/");
  while (!queue.empty()) {
    auto& [buf_ts, buf_values] = queue.front();
    fields_.clear();
    const auto& def = g_tsl_definitions[hash];
    size_t n = std::min(def.size(), buf_values.size());
    for (size_t i = 0; i < n; i++) {
      addFieldAt(def[i], buf_values[i]);
    }
    if (!fields_.empty()) {
      // Ignore error from buffered flush — best effort.
      (void)emitRecord(buf_ts);
    }
    queue.pop();
  }
  fields_.clear();
  // No fields to emit for the definition itself.
}

void RosParser::handleTSLValues() {
  (void)deserializer_->deserializeUInt32();  // stamp sec
  (void)deserializer_->deserializeUInt32();  // stamp nsec
  uint64_t hash = deserializer_->deserialize(RosMsgParser::UINT64).extract<uint64_t>();

  auto& values = statistics_values_;
  values.clear();
  for (auto type_id : kTslTypeOrder) {
    size_t num = readArrayCount(1);  // the smallest TSL type is one byte
    size_t base = values.size();
    values.resize(base + num);
    for (size_t i = base; i < values.size(); i++) {
      values[i] = deserializer_->deserialize(type_id).convert<double>();
    }
  }

  if (g_tsl_definitions.count(hash) == 0) {
    auto& queue = g_tsl_values_buffer[hash];
    if (queue.size() > 1000) {
      queue.pop();  // cap buffer to prevent unbounded growth
    }
    queue.push({current_timestamp_, std::move(values)});
    return;
  }

  const auto& def = g_tsl_definitions[hash];
  size_t n = std::min(def.size(), values.size());
  path_.assign("/");
  for (size_t i = 0; i < n; i++) {
    addFieldAt(def[i], values[i]);
  }
}

}  // namespace ros_parser_detail
