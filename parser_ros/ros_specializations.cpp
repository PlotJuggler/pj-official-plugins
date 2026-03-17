#include "ros_parser_internal.hpp"

#include <data_tamer_parser/data_tamer_parser.hpp>

#include <queue>
#include <tuple>

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
thread_local std::unordered_map<uint64_t, std::queue<std::tuple<int64_t, std::vector<double>>>>
    g_tsl_values_buffer;

// TSL type order (matches old parser)
constexpr std::array<RosMsgParser::BuiltinType, 11> kTslTypeOrder = {
    RosMsgParser::BOOL,    RosMsgParser::INT8,    RosMsgParser::UINT8,
    RosMsgParser::INT16,   RosMsgParser::UINT16,  RosMsgParser::INT32,
    RosMsgParser::UINT32,  RosMsgParser::INT64,   RosMsgParser::UINT64,
    RosMsgParser::FLOAT32, RosMsgParser::FLOAT64,
};

}  // namespace

// ---------------------------------------------------------------------------
// Composition parse helpers
// ---------------------------------------------------------------------------

void RosParser::parseVector3(const std::string& prefix) {
  double x = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  double y = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  double z = deserializer_->deserialize(RosMsgParser::FLOAT64).convert<double>();
  addField(prefix + "/x", x);
  addField(prefix + "/y", y);
  addField(prefix + "/z", z);
}

void RosParser::parsePoint(const std::string& prefix) {
  parseVector3(prefix);  // same wire format: 3 × float64
}

void RosParser::parseQuaternion(const std::string& prefix) {
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

void RosParser::parseTwist(const std::string& prefix) {
  parseVector3(prefix + "/linear");
  parseVector3(prefix + "/angular");
}

void RosParser::parsePose(const std::string& prefix) {
  parseVector3(prefix + "/position");
  parseQuaternion(prefix + "/orientation");
}

void RosParser::parseTransform(const std::string& prefix) {
  parsePoint(prefix + "/translation");
  parseQuaternion(prefix + "/rotation");
}

void RosParser::parsePoseWithCovariance(const std::string& prefix) {
  parsePose(prefix + "/pose");
  parseCovariance<6>(prefix + "/covariance");
}

void RosParser::parseTwistWithCovariance(const std::string& prefix) {
  parseTwist(prefix + "/twist");
  parseCovariance<6>(prefix + "/covariance");
}

// ---------------------------------------------------------------------------
// Top-level specialized handlers
// ---------------------------------------------------------------------------

void RosParser::handleEmpty() {
  addField("/value", 0.0);
}

void RosParser::handlePose() {
  parsePose("");
}

void RosParser::handlePoseStamped() {
  auto h = readHeader();
  emitHeader(h);
  parsePose("/pose");
}

void RosParser::handleTransform() {
  parseTransform("");
}

void RosParser::handleTransformStamped() {
  auto h = readHeader();
  emitHeader(h);
  std::string child_frame_id;
  deserializer_->deserializeString(child_frame_id);
  addStringField("/child_frame_id", child_frame_id);
  parseTransform("/transform");
}

void RosParser::handleImu() {
  auto h = readHeader();
  emitHeader(h);
  parseQuaternion("/orientation");
  parseCovariance<3>("/orientation_covariance");
  parseVector3("/angular_velocity");
  parseCovariance<3>("/angular_velocity_covariance");
  parseVector3("/linear_acceleration");
  parseCovariance<3>("/linear_acceleration_covariance");
}

void RosParser::handleOdometry() {
  auto h = readHeader();
  emitHeader(h);
  std::string child_frame_id;
  deserializer_->deserializeString(child_frame_id);
  addStringField("/child_frame_id", child_frame_id);
  parsePoseWithCovariance("/pose");
  parseTwistWithCovariance("/twist");
}

void RosParser::handleJointState() {
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

void RosParser::handleDiagnosticArray() {
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

void RosParser::handleTFMessage() {
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

// ---------------------------------------------------------------------------
// Cross-message state handlers
// ---------------------------------------------------------------------------

void RosParser::handleDataTamerSchemas() {
  size_t count = deserializer_->deserializeUInt32();
  for (size_t i = 0; i < count; i++) {
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

void RosParser::handleDataTamerSnapshot() {
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

void RosParser::handlePalStatisticsNames() {
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

void RosParser::handlePalStatisticsValues() {
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

void RosParser::handleTSLDefinition() {
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

void RosParser::handleTSLValues() {
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
    auto& queue = g_tsl_values_buffer[hash];
    if (queue.size() > 1000) queue.pop();  // cap buffer to prevent unbounded growth
    queue.push({current_timestamp_, std::move(values)});
    return;
  }

  const auto& def = g_tsl_definitions[hash];
  size_t n = std::min(def.size(), values.size());
  for (size_t i = 0; i < n; i++) {
    addField("/" + def[i], values[i]);
  }
}

}  // namespace ros_parser_detail
