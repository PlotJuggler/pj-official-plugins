#include "../foxglove_protocol.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

using namespace PJ::FoxgloveProtocol;

// --- parseBinaryFrame ---

TEST(FoxgloveProtocolTest, ParseBinaryFrameValid) {
  // Build a valid binary frame: opcode(1) + sub_id(4) + log_time(8) + payload
  std::vector<uint8_t> frame;
  frame.push_back(kMessageDataOpcode);  // opcode = 0x01

  uint32_t sub_id = 42;
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&sub_id),
               reinterpret_cast<uint8_t*>(&sub_id) + 4);

  uint64_t log_time = 1234567890123456789ULL;
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&log_time),
               reinterpret_cast<uint8_t*>(&log_time) + 8);

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
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&sub_id),
               reinterpret_cast<uint8_t*>(&sub_id) + 4);

  uint64_t log_time = 0;
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&log_time),
               reinterpret_cast<uint8_t*>(&log_time) + 8);

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
  EXPECT_EQ(msg, R"({"op":"subscribe","subscriptions":[{"id":1,"channelId":100},{"id":2,"channelId":200},{"id":3,"channelId":300}]})");
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

// --- isUsableChannel ---

TEST(FoxgloveProtocolTest, IsUsableChannelValid) {
  ChannelInfo ch;
  ch.id = 1;
  ch.topic = "/test/topic";
  ch.encoding = "cdr";
  ch.schema_name = "std_msgs/msg/String";
  ch.schema = "string data";
  ch.schema_encoding = "ros2msg";
  EXPECT_TRUE(isUsableChannel(ch));
}

TEST(FoxgloveProtocolTest, IsUsableChannelWrongEncoding) {
  ChannelInfo ch;
  ch.topic = "/test/topic";
  ch.encoding = "json";  // Not CDR
  ch.schema_name = "std_msgs/msg/String";
  ch.schema = "string data";
  ch.schema_encoding = "ros2msg";
  EXPECT_FALSE(isUsableChannel(ch));
}

TEST(FoxgloveProtocolTest, IsUsableChannelWrongSchemaEncoding) {
  ChannelInfo ch;
  ch.topic = "/test/topic";
  ch.encoding = "cdr";
  ch.schema_name = "std_msgs/msg/String";
  ch.schema = "string data";
  ch.schema_encoding = "jsonschema";  // Not ros2msg
  EXPECT_FALSE(isUsableChannel(ch));
}

TEST(FoxgloveProtocolTest, IsUsableChannelEmptyTopic) {
  ChannelInfo ch;
  ch.topic = "";  // Empty
  ch.encoding = "cdr";
  ch.schema_name = "std_msgs/msg/String";
  ch.schema = "string data";
  ch.schema_encoding = "ros2msg";
  EXPECT_FALSE(isUsableChannel(ch));
}

TEST(FoxgloveProtocolTest, IsUsableChannelEmptySchema) {
  ChannelInfo ch;
  ch.topic = "/test/topic";
  ch.encoding = "cdr";
  ch.schema_name = "std_msgs/msg/String";
  ch.schema = "";  // Empty
  ch.schema_encoding = "ros2msg";
  EXPECT_FALSE(isUsableChannel(ch));
}

TEST(FoxgloveProtocolTest, IsUsableChannelEmptySchemaName) {
  ChannelInfo ch;
  ch.topic = "/test/topic";
  ch.encoding = "cdr";
  ch.schema_name = "";  // Empty
  ch.schema = "string data";
  ch.schema_encoding = "ros2msg";
  EXPECT_FALSE(isUsableChannel(ch));
}

}  // namespace
