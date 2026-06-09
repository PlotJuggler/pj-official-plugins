/**
 * @file foxglove_protocol_test.cpp
 * @brief Unit tests for Foxglove WebSocket protocol parsing.
 *
 * These tests verify the binary protocol handling for Foxglove Bridge:
 *   - parseBinaryFrame: decode binary WebSocket messages (opcode + subscription_id
 *     + log_time + payload)
 *   - buildSubscribeMessage / buildUnsubscribeMessage: JSON message generation
 *   - classifyChannel: route a channel by encoding (CDR/ros2msg+omgidl, protobuf)
 *   - pj_base64::decode: shared base64 decoder used for binary (protobuf) schemas
 *
 * All binary frames are constructed in memory as byte vectors. No network
 * connections or WebSocket servers are needed.
 */

#include "../foxglove_protocol.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <pj_base64/base64.hpp>
#include <vector>

namespace {

using namespace PJ::FoxgloveProtocol;

// --- parseBinaryFrame ---

TEST(FoxgloveProtocolTest, ParseBinaryFrameValid) {
  // Build a valid binary frame: opcode(1) + sub_id(4) + log_time(8) + payload
  std::vector<uint8_t> frame;
  frame.push_back(kMessageDataOpcode);  // opcode = 0x01

  uint32_t sub_id = 42;
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&sub_id), reinterpret_cast<uint8_t*>(&sub_id) + 4);

  uint64_t log_time = 1234567890123456789ULL;
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&log_time), reinterpret_cast<uint8_t*>(&log_time) + 8);

  // Payload: "hello"
  const char* payload = "hello";
  frame.insert(frame.end(), payload, payload + 5);

  BinaryFrame out;
  ASSERT_TRUE(parseBinaryFrame(frame.data(), frame.size(), out));
  EXPECT_EQ(out.opcode, kMessageDataOpcode);
  EXPECT_EQ(out.subscription_id, 42u);
  EXPECT_EQ(out.log_time_ns, 1234567890123456789ULL);
  EXPECT_EQ(out.payload_size, 5u);
  EXPECT_EQ(std::memcmp(out.payload_data, "hello", 5), 0);
}

TEST(FoxgloveProtocolTest, ParseBinaryFrameTooSmall) {
  std::vector<uint8_t> frame(kBinaryFrameHeaderSize - 1, 0);
  BinaryFrame out;
  EXPECT_FALSE(parseBinaryFrame(frame.data(), frame.size(), out));
}

TEST(FoxgloveProtocolTest, ParseBinaryFrameWrongOpcode) {
  std::vector<uint8_t> frame(kBinaryFrameHeaderSize, 0);
  frame[0] = 0xFF;  // Invalid opcode
  BinaryFrame out;
  EXPECT_FALSE(parseBinaryFrame(frame.data(), frame.size(), out));
}

TEST(FoxgloveProtocolTest, ParseBinaryFrameEmptyPayload) {
  std::vector<uint8_t> frame;
  frame.push_back(kMessageDataOpcode);

  uint32_t sub_id = 1;
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&sub_id), reinterpret_cast<uint8_t*>(&sub_id) + 4);

  uint64_t log_time = 0;
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&log_time), reinterpret_cast<uint8_t*>(&log_time) + 8);

  BinaryFrame out;
  ASSERT_TRUE(parseBinaryFrame(frame.data(), frame.size(), out));
  EXPECT_EQ(out.payload_size, 0u);
  EXPECT_EQ(out.payload_data, frame.data() + kBinaryFrameHeaderSize);
}

// --- buildSubscribeMessage ---

TEST(FoxgloveProtocolTest, BuildSubscribeMessageSingle) {
  auto msg = buildSubscribeMessage({{1, 100}});
  EXPECT_EQ(msg, R"({"op":"subscribe","subscriptions":[{"id":1,"channelId":100}]})");
}

TEST(FoxgloveProtocolTest, BuildSubscribeMessageMultiple) {
  auto msg = buildSubscribeMessage({{1, 100}, {2, 200}, {3, 300}});
  EXPECT_EQ(
      msg,
      R"({"op":"subscribe","subscriptions":[{"id":1,"channelId":100},{"id":2,"channelId":200},{"id":3,"channelId":300}]})");
}

TEST(FoxgloveProtocolTest, BuildSubscribeMessageEmpty) {
  auto msg = buildSubscribeMessage({});
  EXPECT_EQ(msg, R"({"op":"subscribe","subscriptions":[]})");
}

// --- buildUnsubscribeMessage ---

TEST(FoxgloveProtocolTest, BuildUnsubscribeMessageSingle) {
  auto msg = buildUnsubscribeMessage({42});
  EXPECT_EQ(msg, R"({"op":"unsubscribe","subscriptionIds":[42]})");
}

TEST(FoxgloveProtocolTest, BuildUnsubscribeMessageMultiple) {
  auto msg = buildUnsubscribeMessage({1, 2, 3});
  EXPECT_EQ(msg, R"({"op":"unsubscribe","subscriptionIds":[1,2,3]})");
}

TEST(FoxgloveProtocolTest, BuildUnsubscribeMessageEmpty) {
  auto msg = buildUnsubscribeMessage({});
  EXPECT_EQ(msg, R"({"op":"unsubscribe","subscriptionIds":[]})");
}

// --- classifyChannel ---

TEST(FoxgloveProtocolTest, ClassifyChannelRos2msg) {
  ChannelInfo ch;
  ch.id = 1;
  ch.topic = "/test/topic";
  ch.encoding = "cdr";
  ch.schema_name = "std_msgs/msg/String";
  ch.schema = "string data";
  ch.schema_encoding = "ros2msg";
  auto route = classifyChannel(ch);
  EXPECT_TRUE(route.supported);
  EXPECT_EQ(route.parser_encoding, "ros2msg");
  EXPECT_FALSE(route.schema_is_base64);
}

TEST(FoxgloveProtocolTest, ClassifyChannelOmgIdl) {
  ChannelInfo ch;
  ch.id = 1;
  ch.topic = "/test/topic";
  ch.encoding = "cdr";
  ch.schema_name = "pkg::Simple";
  ch.schema = "module pkg { struct Simple { long value; }; };";
  ch.schema_encoding = "omgidl";
  auto route = classifyChannel(ch);
  EXPECT_TRUE(route.supported);
  EXPECT_EQ(route.parser_encoding, "omgidl");
  EXPECT_FALSE(route.schema_is_base64);
}

TEST(FoxgloveProtocolTest, ClassifyChannelProtobuf) {
  ChannelInfo ch;
  ch.topic = "/camera/image";
  ch.encoding = "protobuf";
  ch.schema_name = "my.pkg.Image";
  ch.schema = "ChJzb21lLWZpbGUtZGVzY3JpcHRvcg==";  // base64 (content irrelevant here)
  ch.schema_encoding = "protobuf";
  auto route = classifyChannel(ch);
  EXPECT_TRUE(route.supported);
  EXPECT_EQ(route.parser_encoding, "protobuf");
  // Must be flagged base64 so the source decodes it before binding.
  EXPECT_TRUE(route.schema_is_base64);
}

TEST(FoxgloveProtocolTest, ClassifyChannelProtobufWellKnownEmptySchema) {
  // Well-known foxglove.* types are bound by name; an empty schema is allowed.
  ChannelInfo ch;
  ch.topic = "/camera/image";
  ch.encoding = "protobuf";
  ch.schema_name = "foxglove.CompressedImage";
  ch.schema = "";
  ch.schema_encoding = "protobuf";
  auto route = classifyChannel(ch);
  EXPECT_TRUE(route.supported);
  EXPECT_EQ(route.parser_encoding, "protobuf");
}

TEST(FoxgloveProtocolTest, ClassifyChannelUnsupportedSchemaEncoding) {
  ChannelInfo ch;
  ch.topic = "/test/topic";
  ch.encoding = "json";
  ch.schema_name = "std_msgs/msg/String";
  ch.schema = "{}";
  ch.schema_encoding = "jsonschema";  // jsonschema/flatbuffer intentionally unsupported
  EXPECT_FALSE(classifyChannel(ch).supported);
}

TEST(FoxgloveProtocolTest, ClassifyChannelCdrButNonRosSchema) {
  ChannelInfo ch;
  ch.topic = "/test/topic";
  ch.encoding = "cdr";
  ch.schema_name = "std_msgs/msg/String";
  ch.schema = "string data";
  ch.schema_encoding = "jsonschema";  // cdr but not ros2msg/omgidl
  EXPECT_FALSE(classifyChannel(ch).supported);
}

TEST(FoxgloveProtocolTest, ClassifyChannelEmptyTopic) {
  ChannelInfo ch;
  ch.topic = "";  // Empty
  ch.encoding = "cdr";
  ch.schema_name = "std_msgs/msg/String";
  ch.schema = "string data";
  ch.schema_encoding = "ros2msg";
  EXPECT_FALSE(classifyChannel(ch).supported);
}

TEST(FoxgloveProtocolTest, ClassifyChannelRos2msgEmptySchemaRejected) {
  ChannelInfo ch;
  ch.topic = "/test/topic";
  ch.encoding = "cdr";
  ch.schema_name = "std_msgs/msg/String";
  ch.schema = "";  // ros2msg requires a schema
  ch.schema_encoding = "ros2msg";
  EXPECT_FALSE(classifyChannel(ch).supported);
}

TEST(FoxgloveProtocolTest, ClassifyChannelEmptySchemaName) {
  ChannelInfo ch;
  ch.topic = "/test/topic";
  ch.encoding = "cdr";
  ch.schema_name = "";  // Empty
  ch.schema = "string data";
  ch.schema_encoding = "ros2msg";
  EXPECT_FALSE(classifyChannel(ch).supported);
}

// --- shared base64 decoder (pj_base64) ---

TEST(Base64Test, KnownVectors) {
  EXPECT_EQ(PJ::base64::decode(""), "");
  EXPECT_EQ(PJ::base64::decode("Zg=="), "f");
  EXPECT_EQ(PJ::base64::decode("Zm8="), "fo");
  EXPECT_EQ(PJ::base64::decode("Zm9v"), "foo");
  EXPECT_EQ(PJ::base64::decode("Zm9vYmFy"), "foobar");
}

TEST(Base64Test, BinarySafe) {
  // Encodes the 4 bytes {0x00, 0xFF, 0x10, 0x00}; the decoder must preserve the
  // embedded NULs (a protobuf FileDescriptorSet is arbitrary binary).
  const std::string decoded = PJ::base64::decode("AP8QAA==");
  ASSERT_EQ(decoded.size(), 4u);
  EXPECT_EQ(static_cast<uint8_t>(decoded[0]), 0x00);
  EXPECT_EQ(static_cast<uint8_t>(decoded[1]), 0xFF);
  EXPECT_EQ(static_cast<uint8_t>(decoded[2]), 0x10);
  EXPECT_EQ(static_cast<uint8_t>(decoded[3]), 0x00);
}

}  // namespace
