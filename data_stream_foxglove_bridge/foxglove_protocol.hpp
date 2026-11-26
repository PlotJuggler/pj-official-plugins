#pragma once

#include <cstdint>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace PJ::FoxgloveProtocol {

/// The WebSocket subprotocols offered to a Foxglove server, which picks one: foxglove_bridge
/// releases built on the Foxglove SDK accept only "foxglove.sdk.v1", the ones before it only
/// "foxglove.websocket.v1". The messages this plugin exchanges are the same under both.
[[nodiscard]] inline std::vector<std::string> subprotocols() {
  return {"foxglove.sdk.v1", "foxglove.websocket.v1"};
}

struct ChannelInfo {
  uint64_t id = 0;
  std::string topic;
  std::string encoding;
  std::string schema_name;
  std::string schema;
  std::string schema_encoding;
};

/// The channels of an "advertise" message. These come straight off the wire: an entry that is
/// not an object or lacks an unsigned `id` is skipped (it could not be subscribed, and as id 0 it
/// would shadow a real channel 0), and another wrong-typed field reads as empty; never throws.
[[nodiscard]] inline std::vector<ChannelInfo> parseAdvertisedChannels(const nlohmann::json& advertise) {
  const auto string_field = [](const nlohmann::json& entry, const char* key) {
    const auto it = entry.find(key);
    return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string{};
  };
  std::vector<ChannelInfo> channels;
  const auto list = advertise.find("channels");
  if (list == advertise.end() || !list->is_array()) {
    return channels;
  }
  for (const auto& entry : *list) {
    if (!entry.is_object()) {
      continue;
    }
    const auto id = entry.find("id");
    if (id == entry.end() || !id->is_number_unsigned()) {
      continue;
    }
    ChannelInfo ch;
    ch.id = id->get<uint64_t>();
    ch.topic = string_field(entry, "topic");
    ch.encoding = string_field(entry, "encoding");
    ch.schema_name = string_field(entry, "schemaName");
    ch.schema = string_field(entry, "schema");
    ch.schema_encoding = string_field(entry, "schemaEncoding");
    channels.push_back(std::move(ch));
  }
  return channels;
}

struct BinaryFrame {
  uint8_t opcode = 0;
  uint32_t subscription_id = 0;
  uint64_t log_time_ns = 0;
  const uint8_t* payload_data = nullptr;
  size_t payload_size = 0;
};

constexpr uint8_t kMessageDataOpcode = 0x01;
constexpr size_t kBinaryFrameHeaderSize = 1 + 4 + 8;  // opcode + sub_id + log_time

/// Parse a binary Foxglove data frame. Returns false if buffer too small or wrong opcode.
inline bool parseBinaryFrame(const uint8_t* data, size_t size, BinaryFrame& out) {
  if (size < kBinaryFrameHeaderSize) {
    return false;
  }

  out.opcode = data[0];
  if (out.opcode != kMessageDataOpcode) {
    return false;
  }

  std::memcpy(&out.subscription_id, data + 1, 4);
  std::memcpy(&out.log_time_ns, data + 5, 8);
  // Assume little-endian (x86/ARM)
  out.payload_data = data + kBinaryFrameHeaderSize;
  out.payload_size = size - kBinaryFrameHeaderSize;
  return true;
}

inline std::string buildSubscribeMessage(const std::vector<std::pair<uint32_t, uint64_t>>& subscriptions) {
  std::string json = R"({"op":"subscribe","subscriptions":[)";
  for (size_t i = 0; i < subscriptions.size(); i++) {
    if (i > 0) {
      json += ',';
    }
    json += R"({"id":)" + std::to_string(subscriptions[i].first) + R"(,"channelId":)" +
            std::to_string(subscriptions[i].second) + '}';
  }
  json += "]}";
  return json;
}

inline std::string buildUnsubscribeMessage(const std::vector<uint32_t>& subscription_ids) {
  std::string json = R"({"op":"unsubscribe","subscriptionIds":[)";
  for (size_t i = 0; i < subscription_ids.size(); i++) {
    if (i > 0) {
      json += ',';
    }
    json += std::to_string(subscription_ids[i]);
  }
  json += "]}";
  return json;
}

/// How a Foxglove channel routes to a message parser.
struct ChannelRoute {
  bool supported = false;
  std::string parser_encoding;      ///< routing key for ensureParserBinding (host picks the parser).
  bool schema_is_base64 = false;    ///< true => the advertised schema must be base64-decoded first.
  bool label_keyed_arrays = false;  ///< parser config: key {label, value} object arrays by label.
};

/// The Foxglove protocol is multi-encoding, so channels are routed per channel (unlike the CDR-native ROS 2 streamer):
///   - CDR + ros2msg/omgidl -> parser_ros (text schema, passed verbatim)
///   - protobuf             -> parser_protobuf (binary FileDescriptorSet schema, base64-encoded in the advertise JSON)
///   - json                 -> parser_json (JSON-Schema text or schemaless; e.g. LeRobot joint/action channels)
/// The protobuf and json routes go beyond the original PlotJuggler 3 plugin, which was CDR/ros2msg-only.
inline ChannelRoute classifyChannel(const ChannelInfo& ch) {
  if (ch.topic.empty()) {
    return {};
  }
  // Text-schema, CDR-serialized ROS 2.
  if (ch.encoding == "cdr" && (ch.schema_encoding == "ros2msg" || ch.schema_encoding == "omgidl") &&
      !ch.schema_name.empty() && !ch.schema.empty()) {
    return ChannelRoute{.supported = true, .parser_encoding = ch.schema_encoding};
  }
  // Base64 FileDescriptorSet schema. It may be empty for the well-known foxglove.* types, which parser_protobuf
  // recognizes by name and decodes without a descriptor.
  if (ch.encoding == "protobuf" && ch.schema_encoding == "protobuf" && !ch.schema_name.empty()) {
    return ChannelRoute{.supported = true, .parser_encoding = "protobuf", .schema_is_base64 = true};
  }
  // The advertised JSON-Schema (plain text, never base64) is informational only: parser_json flattens by content, so
  // schemaless channels work too. Forcing label_keyed_arrays is safe: no saved layout depends on index-based names
  // for json channels of this bridge.
  if (ch.encoding == "json" && (ch.schema_encoding == "jsonschema" || ch.schema_encoding.empty())) {
    return ChannelRoute{.supported = true, .parser_encoding = "json", .label_keyed_arrays = true};
  }
  return {};
}

inline bool isUsableChannel(const ChannelInfo& ch) {
  return classifyChannel(ch).supported;
}

}  // namespace PJ::FoxgloveProtocol
