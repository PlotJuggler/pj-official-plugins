/**
 * @file message_byte_store_test.cpp
 * @brief Tests for mcap::MessageByteStore — the hot/cold lazy message-byte layer
 *        on top of the vendored ParallelReader.
 *
 * The cold path opens the file by path (FileReader), so these tests write a real
 * temporary MCAP file (chunked + zstd) rather than using an in-memory buffer.
 *
 * Coverage:
 *   - hot hit  : fetcher invoked during iteration returns a message-sized copy
 *                with equal bytes (no re-decompress) whose anchor pins only the
 *                message, never the source chunk.
 *   - cold hit : fetcher invoked after the reader is destroyed re-decompresses
 *                and returns equal bytes (covers every message, including
 *                duplicate-(channel,logTime) collisions via the 3-tuple key).
 *   - message-sized copy: two cold reads of the same message yield independent
 *                buffers (not a shared chunk buffer).
 *   - tiny LRU : cold reads stay correct under heavy eviction.
 *   - removed file: cold open fails -> empty, no retry storm.
 *   - source teardown: a fetcher still works after its MessageByteStore owner
 *                is destroyed, matching PJ4's DataSourceHandle lifetime.
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

// Writes a many-chunk fixture for the residency test: `count` messages of
// `payload_bytes` each on one channel, with a small chunkSize so each chunk
// holds only a few messages. The whole decompressed size deliberately dwarfs the
// in-flight byte budget, so a retention regression shows up as inflated peak
// residency. Returns void (uses gtest ASSERTs) like writeFixture().
void writeManyChunkFixture(const std::string& path, size_t count, size_t payload_bytes) {
  mcap::McapWriter writer;
  mcap::McapWriterOptions options("");
  options.compression = mcap::Compression::Zstd;
  options.chunkSize = 4096;  // few messages per chunk -> many chunks
  ASSERT_TRUE(writer.open(path, options).ok());
  mcap::Schema schema("test/Raw", "raw", mcap::ByteArray{});
  writer.addSchema(schema);
  mcap::Channel chan("/topic/big", "raw", schema.id);
  writer.addChannel(chan);
  std::vector<uint8_t> payload(payload_bytes);
  for (size_t i = 0; i < count; ++i) {
    for (size_t b = 0; b < payload_bytes; ++b) {
      payload[b] = static_cast<uint8_t>((i * 131u + b * 7u) & 0xFFu);  // varied, checkable
    }
    mcap::Message m;
    m.channelId = chan.id;
    m.sequence = static_cast<uint32_t>(i);
    m.logTime = 1'000'000'000ULL + i * 1'000'000ULL;
    m.publishTime = m.logTime;
    m.dataSize = payload.size();
    m.data = reinterpret_cast<const std::byte*>(payload.data());
    ASSERT_TRUE(writer.write(m).ok());
  }
  writer.close();
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
  // checks it returns a distinct buffer with equal bytes. Returns the items (fetchers valid for cold use
  // after the reader is destroyed). The reader/source live only for the
  // duration of this call.
  std::vector<Item> collect(mcap::MessageByteStore& store, bool invoke_hot) {
    std::vector<Item> items;
    mcap::ConcurrentFileReader source;
    EXPECT_TRUE(source.open(path_).ok());
    mcap::ParallelReader reader;
    EXPECT_TRUE(reader.open(source).ok());

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
        EXPECT_EQ(v.size, mv.message.dataSize);
        EXPECT_NE(v.data, mv.message.data) << "hot path returns a message copy, not a chunk alias";
        EXPECT_EQ(0, std::memcmp(v.data, mv.message.data, v.size)) << "hot copy must equal the message bytes";
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

TEST_F(MessageByteStoreTest, HotPathReturnsMessageCopy) {
  mcap::MessageByteStore store;
  auto items = collect(store, /*invoke_hot=*/true);
  ASSERT_GE(items.size(), 24u);
}

// Regression: a retained hot ByteView must NOT keep its source chunk resident.
// One MCAP chunk backs ~100 messages, so if the hot anchor pinned the chunk, a
// per-message object topic present in every chunk (e.g. /tf) would pin the whole
// decompressed file. We hold the first message's hot view to the end of
// iteration and assert its source chunk has been freed (weak handle expired)
// while the copied bytes stay valid.
TEST_F(MessageByteStoreTest, HotViewDoesNotPinSourceChunk) {
  mcap::MessageByteStore store;
  mcap::ConcurrentFileReader source;
  ASSERT_TRUE(source.open(path_).ok());
  mcap::ParallelReader reader;
  ASSERT_TRUE(reader.open(source).ok());
  store.init(path_, reader.chunkIndexes());

  mcap::ParallelReadOptions opts;
  opts.read.readOrder = mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;
  opts.maxBytesInFlight = 64ULL * 1024 * 1024;
  opts.threadCount = 2;
  auto on_problem = [](const mcap::Status&) {};

  auto messages = reader.readMessages(on_problem, opts);
  auto it = messages.begin();
  ASSERT_NE(it, messages.end());
  ASSERT_NE(it->message.data, nullptr);

  // First message: capture its chunk liveness handle + a retained hot view.
  std::weak_ptr<const void> first_chunk = it.currentBuffer();
  ASSERT_FALSE(first_chunk.expired()) << "the first message's chunk is live during iteration";
  const auto* p = reinterpret_cast<const uint8_t*>(it->message.data);
  std::vector<uint8_t> expected(p, p + it->message.dataSize);
  mcap::ByteView held = store.makeFetcher(it, *it)();  // hot: a copy of the first message
  ASSERT_NE(held.data, nullptr);

  // Drive iteration to the end so the reader advances past — and frees — the
  // first chunk, while we keep `held` alive throughout.
  size_t count = 1;
  for (++it; it != messages.end(); ++it) {
    ++count;
  }
  ASSERT_GE(count, 24u) << "fixture must span multiple chunks";

  EXPECT_TRUE(first_chunk.expired())
      << "a retained hot view must not pin its source chunk (the chunk-pinning memory bug)";
  ASSERT_EQ(held.size, expected.size());
  EXPECT_EQ(0, std::memcmp(held.data, expected.data(), held.size)) << "the copy outlives its source chunk";
}

// Resource invariant — the scaled regression guard for the chunk-pinning bug.
// Retaining EVERY message's bytes (as the host does for object topics) must not
// grow resident decompressed memory toward the whole file. With the bug each
// retained hot view pinned its chunk, so peak residency equalled the entire
// decompressed dataset; with the fix it tracks the in-flight byte budget. We
// assert on the reader's own peak-residency stat — which nothing else does — and
// that no retained view keeps a chunk alive.
TEST_F(MessageByteStoreTest, RetainingEveryMessageKeepsResidencyBounded) {
  const std::string big = path_ + ".big";
  writeManyChunkFixture(big, /*count=*/200, /*payload_bytes=*/4096);
  struct Cleanup {
    std::string p;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove(p, ec);
    }
  } cleanup{big};

  constexpr uint64_t kBudget = 64ULL * 1024;  // whole decompressed file is ~800 KB
  mcap::MessageByteStore store;
  std::vector<mcap::ByteView> held;  // simulate the host retaining object payloads
  std::vector<std::weak_ptr<const void>> chunks;
  std::vector<uint8_t> first_expected;
  int64_t peak = 0;
  size_t total_payload = 0;
  {
    mcap::ConcurrentFileReader source;
    ASSERT_TRUE(source.open(big).ok());
    mcap::ParallelReader reader;
    ASSERT_TRUE(reader.open(source).ok());
    store.init(big, reader.chunkIndexes());
    mcap::ParallelReadOptions opts;
    opts.read.readOrder = mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;
    opts.maxBytesInFlight = kBudget;
    opts.threadCount = 2;
    auto messages = reader.readMessages([](const mcap::Status&) {}, opts);
    for (auto it = messages.begin(); it != messages.end(); ++it) {
      if (it->message.data == nullptr) {
        continue;
      }
      if (held.empty()) {
        const auto* p = reinterpret_cast<const uint8_t*>(it->message.data);
        first_expected.assign(p, p + it->message.dataSize);
      }
      chunks.push_back(it.currentBuffer());
      mcap::ByteView v = store.makeFetcher(it, *it)();  // hot copy, retained for the whole run
      ASSERT_NE(v.data, nullptr);
      total_payload += v.size;
      held.push_back(std::move(v));
    }
    peak = messages.stats().peakDecompressedBytes.load();
  }
  ASSERT_GE(held.size(), 200u);
  EXPECT_GT(total_payload, 4u * kBudget) << "fixture too small to discriminate";

  // Peak resident decompressed bytes tracked the budget, not the whole file.
  EXPECT_LE(peak, static_cast<int64_t>(kBudget) + 64 * 1024)
      << "retaining every message held " << peak << " B resident (budget " << kBudget << " B)";
  // No retained view kept its source chunk alive.
  size_t pinned = 0;
  for (const auto& w : chunks) {
    if (!w.expired()) {
      ++pinned;
    }
  }
  EXPECT_EQ(pinned, 0u) << pinned << " chunk(s) still pinned by retained hot views";
  // The retained copies are still the right bytes after their chunks are gone.
  ASSERT_EQ(held.front().size, first_expected.size());
  EXPECT_EQ(0, std::memcmp(held.front().data, first_expected.data(), held.front().size));
}

TEST_F(MessageByteStoreTest, ColdPathReproducesEveryMessage) {
  mcap::MessageByteStore store;
  std::vector<Item> items = collect(store, /*invoke_hot=*/false);
  ASSERT_GE(items.size(), 24u);

  // Reader + source are destroyed (collect() returned): all fetches go cold.
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
    mcap::ConcurrentFileReader source;
    ASSERT_TRUE(source.open(path_).ok());
    mcap::ParallelReader reader;
    ASSERT_TRUE(reader.open(source).ok());
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

TEST_F(MessageByteStoreTest, FetcherSurvivesStoreDestroyed) {
  auto store = std::make_unique<mcap::MessageByteStore>();
  std::vector<Item> items = collect(*store, /*invoke_hot=*/false);
  ASSERT_FALSE(items.empty());
  mcap::MessageByteFetcher fetcher = items.front().fetcher;
  std::vector<uint8_t> expected = items.front().expected;

  // Store alive -> cold path works.
  mcap::ByteView before = fetcher();
  ASSERT_NE(before.data, nullptr);
  ASSERT_EQ(before.size, expected.size());
  EXPECT_EQ(0, std::memcmp(before.data, expected.data(), before.size));

  // PJ4 destroys the source plugin handle after import, but ObjectStore lazy
  // closures can be pulled later. The fetcher must keep the cold state alive.
  store.reset();
  mcap::ByteView after = fetcher();
  ASSERT_NE(after.data, nullptr) << "fetcher must survive source teardown";
  ASSERT_EQ(after.size, expected.size());
  EXPECT_EQ(0, std::memcmp(after.data, expected.data(), after.size));
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
