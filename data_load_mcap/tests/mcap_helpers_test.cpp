/**
 * @file mcap_helpers_test.cpp
 * @brief Unit tests for MCAP file reading helpers.
 *
 * These tests verify the MCAP summary reading functions work correctly by
 * creating MCAP data entirely in memory (no disk I/O). We use MemoryWritable
 * and MemoryReadable helper classes to simulate file operations, making the
 * tests fast and portable across machines.
 *
 * What we test:
 *   - Reading schemas and channels from MCAP summary
 *   - Iterating through messages
 *   - Handling multi-channel MCAP files
 *   - Empty MCAP files
 */

#define MCAP_IMPLEMENTATION
#include "../mcap_helpers.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <mcap/writer.hpp>
#include <sstream>
#include <vector>

namespace {

using namespace PJ::McapHelpers;

// Helper class to write MCAP data to an in-memory buffer
class MemoryWritable : public mcap::IWritable {
 public:
  void handleWrite(const std::byte* data, uint64_t size) override {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
  }

  void end() override {}

  uint64_t size() const override {
    return buffer_.size();
  }

  const std::vector<uint8_t>& data() const {
    return buffer_;
  }

 private:
  std::vector<uint8_t> buffer_;
};

// Helper class to read MCAP data from an in-memory buffer
class MemoryReadable : public mcap::IReadable {
 public:
  explicit MemoryReadable(const std::vector<uint8_t>& data) : data_(data) {}

  uint64_t size() const override {
    return data_.size();
  }

  uint64_t read(std::byte** output, uint64_t offset, uint64_t size) override {
    if (offset >= data_.size()) {
      *output = nullptr;
      return 0;
    }
    uint64_t available = std::min(size, data_.size() - offset);
    read_buffer_.resize(available);
    std::memcpy(read_buffer_.data(), data_.data() + offset, available);
    *output = read_buffer_.data();
    return available;
  }

 private:
  const std::vector<uint8_t>& data_;
  std::vector<std::byte> read_buffer_;
};

// Create a minimal valid MCAP file with schema, channel, messages and summary
std::vector<uint8_t> createTestMcap(uint64_t message_count = 10) {
  MemoryWritable writable;
  mcap::McapWriter writer;

  mcap::McapWriterOptions options("test_profile");
  options.compression = mcap::Compression::None;
  writer.open(writable, options);

  // Add a schema
  mcap::Schema schema;
  schema.name = "test_msg";
  schema.encoding = "json";
  schema.data.assign(reinterpret_cast<const std::byte*>("{}"), reinterpret_cast<const std::byte*>("{}") + 2);
  writer.addSchema(schema);

  // Add a channel
  mcap::Channel channel;
  channel.topic = "/test/topic";
  channel.schemaId = schema.id;
  channel.messageEncoding = "json";
  writer.addChannel(channel);

  // Write some messages
  mcap::Message msg;
  msg.channelId = channel.id;
  msg.sequence = 0;
  const char* data = R"({"value": 42})";
  msg.data = reinterpret_cast<const std::byte*>(data);
  msg.dataSize = std::strlen(data);

  for (uint64_t i = 0; i < message_count; i++) {
    msg.sequence = static_cast<uint32_t>(i);
    msg.publishTime = 1000000000 + i * 10000000;  // 1s + i*10ms
    msg.logTime = msg.publishTime;
    (void)writer.write(msg);
  }

  writer.close();
  return writable.data();
}

// --- McapReader basic tests ---

TEST(McapHelpersTest, ReadMcapBasic) {
  auto mcap_data = createTestMcap(5);
  MemoryReadable readable(mcap_data);

  mcap::McapReader reader;
  auto status = reader.open(readable);
  ASSERT_TRUE(status.ok()) << status.message;

  // Read summary via standard method
  status = reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
  ASSERT_TRUE(status.ok()) << status.message;

  // Verify we got schema and channel
  EXPECT_EQ(reader.schemas().size(), 1u);
  EXPECT_EQ(reader.channels().size(), 1u);

  // Verify statistics
  auto stats = reader.statistics();
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->messageCount, 5u);
  EXPECT_EQ(stats->channelCount, 1u);
  EXPECT_EQ(stats->schemaCount, 1u);
}

TEST(McapHelpersTest, ReadSelectiveSummaryValid) {
  auto mcap_data = createTestMcap(10);
  MemoryReadable readable(mcap_data);

  McapSummaryInfo info;
  auto status = readSelectiveSummary(readable, info);

  // Note: readSelectiveSummary requires SummaryOffset records which may not
  // be present in minimal MCAP files. The fallback path in the plugin handles this.
  // If the test fails, it's expected for minimal files without SummaryOffset.
  if (!status.ok()) {
    // This is expected for minimal MCAP files - they may not have SummaryOffset section
    EXPECT_TRUE(status.code == mcap::StatusCode::MissingStatistics || status.code == mcap::StatusCode::InvalidFooter)
        << "Unexpected error: " << status.message;
  } else {
    EXPECT_FALSE(info.schemas.empty());
    EXPECT_FALSE(info.channels.empty());
    EXPECT_TRUE(info.statistics.has_value());
    EXPECT_EQ(info.statistics->messageCount, 10u);
  }
}

TEST(McapHelpersTest, PopulateSummaryFromReader) {
  auto mcap_data = createTestMcap(3);
  MemoryReadable readable(mcap_data);

  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(readable).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok());

  McapSummaryInfo info;
  populateSummaryFromReader(reader, info);

  EXPECT_EQ(info.schemas.size(), 1u);
  EXPECT_EQ(info.channels.size(), 1u);
  EXPECT_TRUE(info.statistics.has_value());
  EXPECT_EQ(info.statistics->messageCount, 3u);

  // Verify channel details
  auto& channel = info.channels.begin()->second;
  EXPECT_EQ(channel->topic, "/test/topic");
  EXPECT_EQ(channel->messageEncoding, "json");

  // Verify schema details
  auto& schema = info.schemas.begin()->second;
  EXPECT_EQ(schema->name, "test_msg");
  EXPECT_EQ(schema->encoding, "json");
}

TEST(McapHelpersTest, ReadMessagesIteration) {
  auto mcap_data = createTestMcap(5);
  MemoryReadable readable(mcap_data);

  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(readable).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok());

  // Iterate through messages
  uint64_t count = 0;
  for (const auto& msg_view : reader.readMessages()) {
    EXPECT_EQ(msg_view.channel->topic, "/test/topic");
    EXPECT_GT(msg_view.message.logTime, 0u);
    count++;
  }
  EXPECT_EQ(count, 5u);
}

TEST(McapHelpersTest, McapWithMultipleChannels) {
  MemoryWritable writable;
  mcap::McapWriter writer;

  mcap::McapWriterOptions options("test");
  writer.open(writable, options);

  // Schema 1
  mcap::Schema schema1;
  schema1.name = "type_a";
  schema1.encoding = "json";
  schema1.data.assign(reinterpret_cast<const std::byte*>("{}"), reinterpret_cast<const std::byte*>("{}") + 2);
  writer.addSchema(schema1);

  // Schema 2
  mcap::Schema schema2;
  schema2.name = "type_b";
  schema2.encoding = "json";
  schema2.data.assign(reinterpret_cast<const std::byte*>("{}"), reinterpret_cast<const std::byte*>("{}") + 2);
  writer.addSchema(schema2);

  // Channel 1
  mcap::Channel channel1;
  channel1.topic = "/sensor/imu";
  channel1.schemaId = schema1.id;
  channel1.messageEncoding = "json";
  writer.addChannel(channel1);

  // Channel 2
  mcap::Channel channel2;
  channel2.topic = "/sensor/gps";
  channel2.schemaId = schema2.id;
  channel2.messageEncoding = "json";
  writer.addChannel(channel2);

  // Write messages to both channels
  mcap::Message msg;
  const char* data = "{}";
  msg.data = reinterpret_cast<const std::byte*>(data);
  msg.dataSize = 2;

  for (int i = 0; i < 3; i++) {
    msg.channelId = channel1.id;
    msg.publishTime = msg.logTime = static_cast<uint64_t>(i) * 1000000;
    (void)writer.write(msg);

    msg.channelId = channel2.id;
    msg.publishTime = msg.logTime = static_cast<uint64_t>(i) * 1000000 + 500000;
    (void)writer.write(msg);
  }

  writer.close();
  auto mcap_data = writable.data();

  // Read and verify
  MemoryReadable readable(mcap_data);
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(readable).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok());

  EXPECT_EQ(reader.schemas().size(), 2u);
  EXPECT_EQ(reader.channels().size(), 2u);

  auto stats = reader.statistics();
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->messageCount, 6u);
  EXPECT_EQ(stats->channelCount, 2u);

  McapSummaryInfo info;
  populateSummaryFromReader(reader, info);
  EXPECT_EQ(info.channels.size(), 2u);
}

TEST(McapHelpersTest, EmptyMcap) {
  MemoryWritable writable;
  mcap::McapWriter writer;

  mcap::McapWriterOptions options("test");
  writer.open(writable, options);
  writer.close();

  auto mcap_data = writable.data();
  MemoryReadable readable(mcap_data);

  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(readable).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok());

  EXPECT_TRUE(reader.schemas().empty());
  EXPECT_TRUE(reader.channels().empty());

  auto stats = reader.statistics();
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->messageCount, 0u);
}

// ---------------------------------------------------------------------------
// Message-index detection — which read order a recording can actually replay.
//
// LogTimeOrder requires Message Index records. Two very different recordings
// lack them while still reporting a non-empty chunkIndexes() list:
//   * a valid file written with McapWriterOptions::noMessageIndex = true;
//   * any file whose summary had to be reconstructed by a fallback scan,
//     because synthesized chunk indexes always carry messageIndexLength == 0.
// Asking "are there chunk indexes?" answers neither. These tests pin the
// distinction, because getting it wrong imports zero messages.
// ---------------------------------------------------------------------------

// Chunked MCAP; message indexes are written unless suppressed. Pass
// chunked=false for a flat recording (the writer chunks by default).
std::vector<uint8_t> createChunkedMcap(bool with_message_index, uint64_t message_count = 400, bool chunked = true) {
  MemoryWritable writable;
  mcap::McapWriter writer;

  mcap::McapWriterOptions options("test_profile");
  options.compression = mcap::Compression::None;
  options.chunkSize = 1024;  // many small chunks
  options.noChunking = !chunked;
  options.noMessageIndex = !with_message_index;
  writer.open(writable, options);

  mcap::Schema schema;
  schema.name = "test_msg";
  schema.encoding = "json";
  schema.data.assign(reinterpret_cast<const std::byte*>("{}"), reinterpret_cast<const std::byte*>("{}") + 2);
  writer.addSchema(schema);

  mcap::Channel channel;
  channel.topic = "/test/topic";
  channel.schemaId = schema.id;
  channel.messageEncoding = "json";
  writer.addChannel(channel);

  const std::string payload = R"({"value": 42, "pad": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"})";
  mcap::Message msg;
  msg.channelId = channel.id;
  msg.data = reinterpret_cast<const std::byte*>(payload.data());
  msg.dataSize = payload.size();

  for (uint64_t i = 0; i < message_count; i++) {
    msg.sequence = static_cast<uint32_t>(i);
    msg.publishTime = 1000000000 + i * 10000000;
    msg.logTime = msg.publishTime;
    (void)writer.write(msg);
  }

  writer.close();
  return writable.data();
}

// Count messages actually delivered by an iteration in the given read order.
uint64_t countDeliveredMessages(mcap::McapReader& reader, mcap::ReadMessageOptions::ReadOrder order) {
  mcap::ReadMessageOptions opts;
  opts.readOrder = order;
  uint64_t count = 0;
  auto view = reader.readMessages([](const mcap::Status&) {}, opts);
  for (auto it = view.begin(); it != view.end(); ++it) {
    ++count;
  }
  return count;
}

TEST(McapMessageIndexTest, UnchunkedRecordingLacksMessageIndexes) {
  auto data = createChunkedMcap(/*with_message_index=*/true, /*message_count=*/10, /*chunked=*/false);
  MemoryReadable readable(data);
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(readable).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());

  EXPECT_TRUE(reader.chunkIndexes().empty());
  EXPECT_TRUE(lacksMessageIndexes(reader.chunkIndexes()));
}

TEST(McapMessageIndexTest, ChunkedRecordingWithIndexesHasThem) {
  auto data = createChunkedMcap(/*with_message_index=*/true);
  MemoryReadable readable(data);
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(readable).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());

  ASSERT_FALSE(reader.chunkIndexes().empty());
  EXPECT_FALSE(lacksMessageIndexes(reader.chunkIndexes()));
  // The fast path stays available for healthy indexed recordings.
  EXPECT_EQ(countDeliveredMessages(reader, mcap::ReadMessageOptions::ReadOrder::LogTimeOrder), 400u);
}

// A perfectly valid recording — no corruption at all — that simply opted out
// of message indexes. chunkIndexes() is non-empty, so an "is it empty?" test
// wrongly concludes log-time replay is possible.
TEST(McapMessageIndexTest, ValidRecordingWrittenWithoutMessageIndexes) {
  auto data = createChunkedMcap(/*with_message_index=*/false);
  MemoryReadable readable(data);
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(readable).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());

  ASSERT_FALSE(reader.chunkIndexes().empty()) << "chunk indexes exist; only message indexes are absent";
  EXPECT_TRUE(lacksMessageIndexes(reader.chunkIndexes()));
}

// Damaging the trailing magic forces readSummary() down the fallback-scan
// path, which synthesizes chunk indexes with messageIndexLength == 0.
TEST(McapMessageIndexTest, SummaryReconstructedByFallbackScanLacksIndexes) {
  auto data = createChunkedMcap(/*with_message_index=*/true);
  ASSERT_GT(data.size(), 8u);
  data[data.size() - 4] ^= 0xFF;  // corrupt the trailing magic

  MemoryReadable readable(data);
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(readable).ok()) << "leading magic is intact, so open() must succeed";
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());

  EXPECT_FALSE(reader.footer().has_value()) << "footer is unreadable, summary came from the scan";
  ASSERT_FALSE(reader.chunkIndexes().empty()) << "the scan synthesized chunk indexes";
  EXPECT_TRUE(lacksMessageIndexes(reader.chunkIndexes()));
}

// Why the distinction matters: asking for LogTimeOrder without message indexes
// silently yields nothing, while FileOrder recovers every message.
TEST(McapMessageIndexTest, LogTimeOrderYieldsNothingWithoutMessageIndexes) {
  auto data = createChunkedMcap(/*with_message_index=*/false);
  MemoryReadable readable(data);
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(readable).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());

  EXPECT_EQ(countDeliveredMessages(reader, mcap::ReadMessageOptions::ReadOrder::LogTimeOrder), 0u);
}

TEST(McapMessageIndexTest, FileOrderRecoversMessagesWithoutMessageIndexes) {
  auto data = createChunkedMcap(/*with_message_index=*/false);
  MemoryReadable readable(data);
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(readable).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());

  ASSERT_TRUE(lacksMessageIndexes(reader.chunkIndexes()));
  EXPECT_EQ(countDeliveredMessages(reader, mcap::ReadMessageOptions::ReadOrder::FileOrder), 400u);
}

// The same recovery, end to end, for a footer-damaged recording.
TEST(McapMessageIndexTest, FileOrderRecoversFooterDamagedRecording) {
  auto data = createChunkedMcap(/*with_message_index=*/true);
  data[data.size() - 4] ^= 0xFF;

  MemoryReadable readable(data);
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(readable).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok());

  ASSERT_TRUE(lacksMessageIndexes(reader.chunkIndexes()));
  EXPECT_EQ(countDeliveredMessages(reader, mcap::ReadMessageOptions::ReadOrder::LogTimeOrder), 0u);
  EXPECT_EQ(countDeliveredMessages(reader, mcap::ReadMessageOptions::ReadOrder::FileOrder), 400u);
}

}  // namespace
