#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace PJ::FoxgloveProtocol {

struct ChannelInfo {
  uint64_t id = 0;
  std::string topic;
  std::string encoding;
  std::string schema_name;
  std::string schema;
  std::string schema_encoding;
};

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

/// Build a JSON subscribe message for given subscription IDs and channel IDs.
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

/// Build a JSON unsubscribe message.
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
  std::string parser_encoding;    ///< routing key for ensureParserBinding (host picks the parser).
  bool schema_is_base64 = false;  ///< true => the advertised schema must be base64-decoded first.
};

/// Classify a Foxglove channel into a parser route.
///
/// The Foxglove WebSocket protocol is multi-encoding. Unlike the ROS2 streamer
/// (CDR-native by construction), this bridge dispatches per channel:
///   - CDR + ros2msg/omgidl -> parser_ros (text schema, passed verbatim)
///   - protobuf             -> parser_protobuf (binary FileDescriptorSet schema,
///                             base64-encoded in the advertise JSON)
///
/// NOTE: this intentionally extends beyond the upstream PlotJuggler plugin,
/// which was CDR/ros2msg-only. The protobuf route is an additive enhancement
/// (see data_stream_foxglove_bridge/README.md). Future encodings (jsonschema,
/// flatbuffer) are a single new branch each.
inline ChannelRoute classifyChannel(const ChannelInfo& ch) {
  if (ch.topic.empty() || ch.schema_name.empty()) {
    return {};
  }
  // Text-schema, CDR-serialized ROS 2 (the original behaviour).
  if (ch.encoding == "cdr" && (ch.schema_encoding == "ros2msg" || ch.schema_encoding == "omgidl") &&
      !ch.schema.empty()) {
    return {true, ch.schema_encoding, /*schema_is_base64=*/false};
  }
  // Protobuf wire messages with a base64 FileDescriptorSet schema. The schema
  // MAY be empty for the well-known foxglove.* types, which parser_protobuf
  // recognizes by name and decodes without a descriptor.
  if (ch.encoding == "protobuf" && ch.schema_encoding == "protobuf") {
    return {true, "protobuf", /*schema_is_base64=*/true};
  }
  return {};
}

/// Convenience predicate retained for call sites that only need yes/no.
inline bool isUsableChannel(const ChannelInfo& ch) {
  return classifyChannel(ch).supported;
}

/// Result of one declarative subscription reconcile (lazy subscription).
struct SubscriptionOps {
  std::vector<std::pair<uint32_t, uint64_t>> to_subscribe;  ///< (fresh sub_id, channel_id)
  std::vector<uint32_t> to_unsubscribe;                     ///< sub ids to drop
};

/// Pure diff between the live subscription map (sub_id -> channel_id) and the
/// DESIRED channel-id set: channels newly desired get fresh sub ids from
/// `next_sub_id`; subscriptions whose channel is no longer desired are
/// dropped. Idempotent: an empty diff yields empty ops.
inline SubscriptionOps computeSubscriptionOps(
    const std::map<uint32_t, uint64_t>& current, const std::set<uint64_t>& desired_channels, uint32_t& next_sub_id) {
  SubscriptionOps ops;
  std::set<uint64_t> covered;
  for (const auto& [sub_id, channel_id] : current) {
    if (desired_channels.count(channel_id) == 0) {
      ops.to_unsubscribe.push_back(sub_id);
    } else {
      covered.insert(channel_id);
    }
  }
  for (const uint64_t channel_id : desired_channels) {
    if (covered.count(channel_id) == 0) {
      ops.to_subscribe.emplace_back(next_sub_id++, channel_id);
    }
  }
  return ops;
}

}  // namespace PJ::FoxgloveProtocol
