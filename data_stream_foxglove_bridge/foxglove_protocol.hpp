#pragma once

#include <cstdint>
#include <cstring>
#include <string>
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
  if (size < kBinaryFrameHeaderSize) return false;

  out.opcode = data[0];
  if (out.opcode != kMessageDataOpcode) return false;

  std::memcpy(&out.subscription_id, data + 1, 4);
  std::memcpy(&out.log_time_ns, data + 5, 8);
  // Assume little-endian (x86/ARM)
  out.payload_data = data + kBinaryFrameHeaderSize;
  out.payload_size = size - kBinaryFrameHeaderSize;
  return true;
}

/// Build a JSON subscribe message for given subscription IDs and channel IDs.
inline std::string buildSubscribeMessage(
    const std::vector<std::pair<uint32_t, uint64_t>>& subscriptions) {
  std::string json = R"({"op":"subscribe","subscriptions":[)";
  for (size_t i = 0; i < subscriptions.size(); i++) {
    if (i > 0) json += ',';
    json += R"({"id":)" + std::to_string(subscriptions[i].first) +
            R"(,"channelId":)" + std::to_string(subscriptions[i].second) + '}';
  }
  json += "]}";
  return json;
}

/// Build a JSON unsubscribe message.
inline std::string buildUnsubscribeMessage(const std::vector<uint32_t>& subscription_ids) {
  std::string json = R"({"op":"unsubscribe","subscriptionIds":[)";
  for (size_t i = 0; i < subscription_ids.size(); i++) {
    if (i > 0) json += ',';
    json += std::to_string(subscription_ids[i]);
  }
  json += "]}";
  return json;
}

/// Check if a channel is a CDR-encoded ROS2 stream this plugin can handle.
inline bool isUsableChannel(const ChannelInfo& ch) {
  return ch.encoding == "cdr" && ch.schema_encoding == "ros2msg" &&
         !ch.schema.empty() && !ch.schema_name.empty() && !ch.topic.empty();
}

}  // namespace PJ::FoxgloveProtocol
