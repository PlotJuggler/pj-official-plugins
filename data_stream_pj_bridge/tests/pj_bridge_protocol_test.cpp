/**
 * @file pj_bridge_protocol_test.cpp
 * @brief Unit tests for PJ Bridge binary protocol parsing.
 *
 * These tests verify the ZSTD-compressed binary frame protocol used by
 * the PlotJuggler Bridge streaming source:
 *   - parseBinaryFrame: decompress ZSTD payload and extract messages
 *     (topic_name + timestamp_ns + CDR data)
 *   - buildRequest: generate JSON command messages
 *   - generateRequestId: create unique UUID-format request IDs
 *
 * The buildTestFrame() helper creates valid compressed frames in memory
 * using ZSTD compression. No network connections or WebSocket servers
 * are required.
 */

#include "../pj_bridge_protocol.hpp"

#include <gtest/gtest.h>
#include <zstd.h>

#include <cstring>
#include <vector>

namespace {

using namespace PJ::BridgeProtocol;

// Helper to build a valid compressed binary frame for testing.
std::vector<uint8_t> buildTestFrame(const std::vector<RawMessage>& messages) {
  // Build uncompressed payload
  std::vector<uint8_t> payload;
  for (const auto& msg : messages) {
    // topic_len (2 bytes)
    auto topic_len = static_cast<uint16_t>(msg.topic_name.size());
    payload.insert(payload.end(), reinterpret_cast<uint8_t*>(&topic_len),
                   reinterpret_cast<uint8_t*>(&topic_len) + 2);

    // topic (N bytes)
    payload.insert(payload.end(), msg.topic_name.begin(), msg.topic_name.end());

    // timestamp_ns (8 bytes)
    auto ts = static_cast<uint64_t>(msg.timestamp_ns);
    payload.insert(payload.end(), reinterpret_cast<uint8_t*>(&ts),
                   reinterpret_cast<uint8_t*>(&ts) + 8);

    // cdr_len (4 bytes)
    auto cdr_len = static_cast<uint32_t>(msg.cdr_size);
    payload.insert(payload.end(), reinterpret_cast<uint8_t*>(&cdr_len),
                   reinterpret_cast<uint8_t*>(&cdr_len) + 4);

    // cdr_data (M bytes)
    if (msg.cdr_data && msg.cdr_size > 0) {
      payload.insert(payload.end(), msg.cdr_data, msg.cdr_data + msg.cdr_size);
    }
  }

  // Compress with ZSTD
  size_t compress_bound = ZSTD_compressBound(payload.size());
  std::vector<uint8_t> compressed(compress_bound);
  size_t compressed_size =
      ZSTD_compress(compressed.data(), compress_bound, payload.data(), payload.size(), 1);
  compressed.resize(compressed_size);

  // Build header: magic(4) + msg_count(4) + uncompressed_size(4) + flags(4)
  std::vector<uint8_t> frame;

  uint32_t magic = kMagic;
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&magic),
               reinterpret_cast<uint8_t*>(&magic) + 4);

  auto msg_count = static_cast<uint32_t>(messages.size());
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&msg_count),
               reinterpret_cast<uint8_t*>(&msg_count) + 4);

  auto uncompressed_size = static_cast<uint32_t>(payload.size());
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&uncompressed_size),
               reinterpret_cast<uint8_t*>(&uncompressed_size) + 4);

  uint32_t flags = 0;
  frame.insert(frame.end(), reinterpret_cast<uint8_t*>(&flags),
               reinterpret_cast<uint8_t*>(&flags) + 4);

  // Append compressed payload
  frame.insert(frame.end(), compressed.begin(), compressed.end());

  return frame;
}

// --- parseBinaryFrame ---

TEST(PjBridgeProtocolTest, ParseBinaryFrameValid) {
  // Create test data
  const uint8_t cdr_data[] = {0x01, 0x02, 0x03, 0x04};
  RawMessage input_msg;
  input_msg.topic_name = "/test/topic";
  input_msg.timestamp_ns = 1234567890;
  input_msg.cdr_data = cdr_data;
  input_msg.cdr_size = sizeof(cdr_data);

  auto frame = buildTestFrame({input_msg});

  std::vector<RawMessage> messages;
  std::vector<uint8_t> decompress_buffer;
  ASSERT_TRUE(parseBinaryFrame(frame.data(), frame.size(), messages, decompress_buffer));

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].topic_name, "/test/topic");
  EXPECT_EQ(messages[0].timestamp_ns, 1234567890);
  EXPECT_EQ(messages[0].cdr_size, 4u);
  EXPECT_EQ(std::memcmp(messages[0].cdr_data, cdr_data, 4), 0);
}

TEST(PjBridgeProtocolTest, ParseBinaryFrameMultipleMessages) {
  const uint8_t cdr1[] = {0xAA, 0xBB};
  const uint8_t cdr2[] = {0xCC, 0xDD, 0xEE};

  RawMessage msg1;
  msg1.topic_name = "/topic/one";
  msg1.timestamp_ns = 100;
  msg1.cdr_data = cdr1;
  msg1.cdr_size = sizeof(cdr1);

  RawMessage msg2;
  msg2.topic_name = "/topic/two";
  msg2.timestamp_ns = 200;
  msg2.cdr_data = cdr2;
  msg2.cdr_size = sizeof(cdr2);

  auto frame = buildTestFrame({msg1, msg2});

  std::vector<RawMessage> messages;
  std::vector<uint8_t> decompress_buffer;
  ASSERT_TRUE(parseBinaryFrame(frame.data(), frame.size(), messages, decompress_buffer));

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].topic_name, "/topic/one");
  EXPECT_EQ(messages[0].timestamp_ns, 100);
  EXPECT_EQ(messages[1].topic_name, "/topic/two");
  EXPECT_EQ(messages[1].timestamp_ns, 200);
}

TEST(PjBridgeProtocolTest, ParseBinaryFrameTooSmall) {
  std::vector<uint8_t> frame(15, 0);  // Less than 16-byte header

  std::vector<RawMessage> messages;
  std::vector<uint8_t> decompress_buffer;
  EXPECT_FALSE(parseBinaryFrame(frame.data(), frame.size(), messages, decompress_buffer));
}

TEST(PjBridgeProtocolTest, ParseBinaryFrameWrongMagic) {
  auto frame = buildTestFrame({});
  // Corrupt the magic
  frame[0] = 0xFF;

  std::vector<RawMessage> messages;
  std::vector<uint8_t> decompress_buffer;
  EXPECT_FALSE(parseBinaryFrame(frame.data(), frame.size(), messages, decompress_buffer));
}

TEST(PjBridgeProtocolTest, ParseBinaryFrameNonZeroFlags) {
  auto frame = buildTestFrame({});
  // Set flags to non-zero (at offset 12)
  frame[12] = 0x01;

  std::vector<RawMessage> messages;
  std::vector<uint8_t> decompress_buffer;
  EXPECT_FALSE(parseBinaryFrame(frame.data(), frame.size(), messages, decompress_buffer));
}

TEST(PjBridgeProtocolTest, ParseBinaryFrameEmptyMessages) {
  auto frame = buildTestFrame({});

  std::vector<RawMessage> messages;
  std::vector<uint8_t> decompress_buffer;
  ASSERT_TRUE(parseBinaryFrame(frame.data(), frame.size(), messages, decompress_buffer));
  EXPECT_TRUE(messages.empty());
}

// --- buildRequest ---

TEST(PjBridgeProtocolTest, BuildRequestListTopics) {
  auto req = buildRequest("list_topics", "abc-123");
  EXPECT_EQ(req, R"({"command":"list_topics","id":"abc-123","protocol_version":1})");
}

TEST(PjBridgeProtocolTest, BuildRequestSubscribe) {
  auto req = buildRequest("subscribe", "xyz-789");
  EXPECT_EQ(req, R"({"command":"subscribe","id":"xyz-789","protocol_version":1})");
}

// --- generateRequestId ---

TEST(PjBridgeProtocolTest, GenerateRequestIdFormat) {
  auto id = generateRequestId();
  // UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars)
  EXPECT_EQ(id.size(), 36u);
  EXPECT_EQ(id[8], '-');
  EXPECT_EQ(id[13], '-');
  EXPECT_EQ(id[18], '-');
  EXPECT_EQ(id[23], '-');
}

TEST(PjBridgeProtocolTest, GenerateRequestIdUnique) {
  auto id1 = generateRequestId();
  auto id2 = generateRequestId();
  auto id3 = generateRequestId();
  EXPECT_NE(id1, id2);
  EXPECT_NE(id2, id3);
  EXPECT_NE(id1, id3);
}

}  // namespace
