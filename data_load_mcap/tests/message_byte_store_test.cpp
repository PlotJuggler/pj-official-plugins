/**
 * @file message_byte_store_test.cpp
 * @brief Tests for mcap::MessageByteStore — the hot/cold lazy message-byte layer
 *        on top of the vendored ParallelReader.
 *
 * The cold path opens the file by path (FileReader), so these tests write a real
 * temporary MCAP file (chunked + zstd) rather than using an in-memory buffer.
 *
 * Coverage:
 *   - hot hit  : fetcher invoked during iteration returns a zero-copy view
 *                (pointer-identical to message.data — no re-decompress).
 *   - cold hit : fetcher invoked after the reader is destroyed re-decompresses
 *                and returns equal bytes (covers every message, including
 *                duplicate-(channel,logTime) collisions via the 3-tuple key).
 *   - message-sized copy: two cold reads of the same message yield independent
 *                buffers (not a shared chunk buffer).
 *   - tiny LRU : cold reads stay correct under heavy eviction.
 *   - removed file: cold open fails -> empty, no retry storm.
 *   - dead flag: a fetcher fast-fails empty once its store is destroyed.
 *   - concurrency: many cold reads from several threads stay correct.
 *   - init early-warning: a non-readable path reports once via the callback.
 */

#define MCAP_IMPLEMENTATION
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mcap/message_byte_store.hpp>
#include <mcap/parallel_reader.hpp>
#include <mcap/writer.hpp>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// One message's ground truth + the fetcher bound to it during iteration.
struct Item {
  mcap::ChannelId channel_id = 0;
  mcap::Timestamp log_time = 0;
  std::vector<uint8_t> expected;
  mcap::MessageByteFetcher fetcher;
};

std::vector<uint8_t> makePayload(unsigned channel, unsigned seq) {
  // Distinct, variable-length content so a mis-mapped fetch is caught.
  std::string s = "payload|ch=" + std::to_string(channel) + "|seq=" + std::to_string(seq) + "|";
  s.append(seq % 7 + 1, static_cast<char>('A' + (seq % 26)));
  return std::vector<uint8_t>(s.begin(), s.end());
}

class MessageByteStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<int> counter{0};
    path_ = (std::filesystem::temp_directory_path() / ("pj_mbs_" + std::to_string(counter.fetch_add(1)) + ".mcap"))
                .string();
    writeFixture();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  // Writes a chunked, zstd-compressed MCAP with two channels and a deliberate
  // duplicate-(channel, logTime) collision. Small chunkSize forces several
  // chunks. Records ground-truth payloads keyed by (channel_id, log_time).
  void writeFixture() {
    mcap::McapWriter writer;
    mcap::McapWriterOptions options("");
    options.compression = mcap::Compression::Zstd;
    options.chunkSize = 256;  // force multiple chunks across ~24 messages
    ASSERT_TRUE(writer.open(path_, options).ok());

    mcap::Schema schema("test/Raw", "raw", mcap::ByteArray{});
    writer.addSchema(schema);
    mcap::Channel chan_a("/topic/a", "raw", schema.id);
    writer.addChannel(chan_a);
    mcap::Channel chan_b("/topic/b", "raw", schema.id);
    writer.addChannel(chan_b);
    channel_a_ = chan_a.id;
    channel_b_ = chan_b.id;

    auto emit = [&](mcap::ChannelId ch, uint32_t seq, mcap::Timestamp ts) {
      auto payload = makePayload(static_cast<unsigned>(ch), seq);
      mcap::Message m;
      m.channelId = ch;
      m.sequence = seq;
      m.logTime = ts;
      m.publishTime = ts;
      m.dataSize = payload.size();
      m.data = reinterpret_cast<const std::byte*>(payload.data());
      ASSERT_TRUE(writer.write(m).ok());
    };

    uint32_t seq = 0;
    for (uint32_t i = 0; i < 12; ++i) {
      const mcap::Timestamp ts = 1'000'000'000ULL + i * 1'000'000ULL;
      emit(channel_a_, seq++, ts);
      emit(channel_b_, seq++, ts);
    }
    // Deliberate collision: two messages on channel A sharing one logTime, with
    // different payloads. Exercises the within-chunk third key element.
    emit(channel_a_, seq++, kCollisionTs);
    emit(channel_a_, seq++, kCollisionTs);

    writer.close();
  }

  // Iterate via the parallel reader, building a fetcher per message. If
  // `invoke_hot` is true, also invokes each fetcher immediately (hot path) and
  // checks zero-copy identity. Returns the items (fetchers valid for cold use
  // after the reader is destroyed). The reader/source live only for the
  // duration of this call.
  std::vector<Item> collect(mcap::MessageByteStore& store, bool invoke_hot) {
    std::vector<Item> items;
    mcap::MmapReader mmap;
    EXPECT_TRUE(mmap.open(path_).ok());
    mcap::ParallelReader reader;
    EXPECT_TRUE(reader.open(mmap).ok());

    store.init(path_, reader.chunkIndexes());

    mcap::ParallelReadOptions opts;
    opts.read.readOrder = mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;
    opts.maxBytesInFlight = 64ULL * 1024 * 1024;
    opts.threadCount = 2;
    auto on_problem = [](const mcap::Status&) {};

    auto messages = reader.readMessages(on_problem, opts);
    for (auto it = messages.begin(); it != messages.end(); ++it) {
      const auto& mv = *it;
      if (mv.channel == nullptr || mv.message.data == nullptr) {
        continue;
      }
      Item item;
      item.channel_id = mv.channel->id;
      item.log_time = mv.message.logTime;
      const auto* p = reinterpret_cast<const uint8_t*>(mv.message.data);
      item.expected.assign(p, p + mv.message.dataSize);
      item.fetcher = store.makeFetcher(it, mv);
      if (invoke_hot) {
        mcap::ByteView v = item.fetcher();
        EXPECT_EQ(v.data, mv.message.data) << "hot path should be zero-copy";
        EXPECT_EQ(v.size, mv.message.dataSize);
      }
      items.push_back(std::move(item));
    }
    return items;
  }

  static constexpr mcap::Timestamp kCollisionTs = 9'000'000'000ULL;
  std::string path_;
  mcap::ChannelId channel_a_ = 0;
  mcap::ChannelId channel_b_ = 0;
};

TEST_F(MessageByteStoreTest, HotPathZeroCopy) {
  mcap::MessageByteStore store;
  auto items = collect(store, /*invoke_hot=*/true);
  ASSERT_GE(items.size(), 24u);
}

TEST_F(MessageByteStoreTest, ColdPathReproducesEveryMessage) {
  mcap::MessageByteStore store;
  std::vector<Item> items = collect(store, /*invoke_hot=*/false);
  ASSERT_GE(items.size(), 24u);

  // Reader + mmap are destroyed (collect() returned): all fetches go cold.
  size_t collisions = 0;
  for (const auto& item : items) {
    if (item.log_time == kCollisionTs) {
      ++collisions;
    }
    mcap::ByteView v = item.fetcher();
    ASSERT_NE(v.data, nullptr) << "cold fetch returned empty";
    ASSERT_EQ(v.size, item.expected.size());
    EXPECT_EQ(0, std::memcmp(v.data, item.expected.data(), v.size));
  }
  EXPECT_EQ(collisions, 2u) << "fixture must exercise duplicate (channel, logTime)";
}

TEST_F(MessageByteStoreTest, ColdReturnsIndependentMessageSizedCopies) {
  mcap::MessageByteStore store;
  std::vector<Item> items = collect(store, /*invoke_hot=*/false);
  ASSERT_FALSE(items.empty());

  // Two cold reads of the SAME message must yield distinct buffers: a
  // chunk-anchored span would return the same pointer both times.
  mcap::ByteView a = items.front().fetcher();
  mcap::ByteView b = items.front().fetcher();
  ASSERT_NE(a.data, nullptr);
  ASSERT_NE(b.data, nullptr);
  EXPECT_NE(a.data, b.data) << "cold path must return a per-message copy";
  ASSERT_EQ(a.size, b.size);
  EXPECT_EQ(0, std::memcmp(a.data, b.data, a.size));
}

TEST_F(MessageByteStoreTest, TinyCacheStaysCorrectUnderEviction) {
  mcap::MessageByteStore store;
  std::vector<Item> items;
  {
    mcap::MmapReader mmap;
    ASSERT_TRUE(mmap.open(path_).ok());
    mcap::ParallelReader reader;
    ASSERT_TRUE(reader.open(mmap).ok());
    // 1-byte budget: every cold miss evicts the previous chunk immediately.
    store.init(path_, reader.chunkIndexes(), {.cacheCapacityBytes = 1});
    mcap::ParallelReadOptions opts;
    opts.read.readOrder = mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;
    auto on_problem = [](const mcap::Status&) {};
    auto messages = reader.readMessages(on_problem, opts);
    for (auto it = messages.begin(); it != messages.end(); ++it) {
      const auto& mv = *it;
      if (mv.channel == nullptr || mv.message.data == nullptr) {
        continue;
      }
      Item item;
      const auto* p = reinterpret_cast<const uint8_t*>(mv.message.data);
      item.expected.assign(p, p + mv.message.dataSize);
      item.fetcher = store.makeFetcher(it, mv);
      items.push_back(std::move(item));
    }
  }
  for (const auto& item : items) {
    mcap::ByteView v = item.fetcher();
    ASSERT_NE(v.data, nullptr);
    ASSERT_EQ(v.size, item.expected.size());
    EXPECT_EQ(0, std::memcmp(v.data, item.expected.data(), v.size));
  }
}

TEST_F(MessageByteStoreTest, RemovedFileFailsClosedNoRetryStorm) {
  mcap::MessageByteStore store;
  std::vector<Item> items = collect(store, /*invoke_hot=*/false);
  ASSERT_FALSE(items.empty());

  // Delete the file before any cold open happens (open is lazy on first miss).
  std::error_code ec;
  std::filesystem::remove(path_, ec);
  ASSERT_FALSE(ec);

  mcap::ByteView v1 = items.front().fetcher();
  EXPECT_EQ(v1.data, nullptr);
  // Latched failure: a second call must also fail fast (and not crash).
  mcap::ByteView v2 = items.front().fetcher();
  EXPECT_EQ(v2.data, nullptr);
}

TEST_F(MessageByteStoreTest, FetcherFastFailsAfterStoreDestroyed) {
  auto store = std::make_unique<mcap::MessageByteStore>();
  std::vector<Item> items = collect(*store, /*invoke_hot=*/false);
  ASSERT_FALSE(items.empty());
  mcap::MessageByteFetcher fetcher = items.front().fetcher;

  // Store alive -> cold path works.
  mcap::ByteView before = fetcher();
  ASSERT_NE(before.data, nullptr);

  // Destroying the store marks the (still shared-alive) cold state dead.
  store.reset();
  mcap::ByteView after = fetcher();
  EXPECT_EQ(after.data, nullptr) << "fetcher must fast-fail once its store is gone";
}

TEST_F(MessageByteStoreTest, ConcurrentColdReads) {
  mcap::MessageByteStore store;
  std::vector<Item> items = collect(store, /*invoke_hot=*/false);
  ASSERT_GE(items.size(), 24u);

  std::atomic<int> mismatches{0};
  auto worker = [&items, &mismatches](size_t begin, size_t step) {
    for (size_t i = begin; i < items.size(); i += step) {
      mcap::ByteView v = items[i].fetcher();
      if (v.data == nullptr || v.size != items[i].expected.size() ||
          std::memcmp(v.data, items[i].expected.data(), v.size) != 0) {
        mismatches.fetch_add(1);
      }
    }
  };
  std::thread t0(worker, 0, 2);
  std::thread t1(worker, 1, 2);
  t0.join();
  t1.join();
  EXPECT_EQ(mismatches.load(), 0);
}

TEST(MessageByteStoreInit, NonReadablePathReportsOnce) {
  int count = 0;
  mcap::MessageByteStore store;
  store.init("/definitely/does/not/exist/pj_mbs_missing.mcap", {}, {}, [&count](const mcap::Status&) { ++count; });
  EXPECT_EQ(count, 1);
}

}  // namespace
