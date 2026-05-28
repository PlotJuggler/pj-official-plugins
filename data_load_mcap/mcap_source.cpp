#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>

#define MCAP_IMPLEMENTATION
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mcap_dialog.hpp"
#include "mcap_helpers.hpp"
#include "mcap_manifest.hpp"

// Vendored parallel-reader fork (contrib/mcap/). Pulled in after reader.hpp
// so MCAP_IMPLEMENTATION already lit up reader.inl; parallel reader is fully
// inline and stacks on top.
#include <mcap/parallel_reader.hpp>  // NOLINT(build/include_order)

namespace {

using McapSummaryInfo = PJ::McapHelpers::McapSummaryInfo;
using PJ::McapHelpers::populateSummaryFromReader;
using PJ::McapHelpers::readSelectiveSummary;

// LRU cache of decompressed MCAP chunks, bounded by total bytes held.
//
// On miss: byte-bounded LinearMessageView decompresses the target chunk
// directly (no O(chunks_before_target) scan from the data section start),
// then we walk it once and concatenate every message into a single buffer,
// indexed by (channel_id, log_time). Per-pull cost on miss = 1 decompress +
// 1 chunk-sized memcpy.
//
// On hit: O(log n) lookup in the per-chunk index; returns a Span into the
// cached buffer with the buffer's shared_ptr as anchor. Zero-copy through
// to the consumer (plotjuggler_core >=0.5 preserves the anchor end-to-end).
class ChunkCache {
 public:
  using Key = std::pair<mcap::ChannelId, mcap::Timestamp>;

  struct Entry {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    std::map<Key, std::pair<size_t /*offset*/, size_t /*size*/>> index;
  };

  explicit ChunkCache(size_t capacity_bytes) : capacity_bytes_(capacity_bytes) {}

  // Fetcher closures run on consumer threads. The fast path (cache hit) and
  // the bookkeeping path (insert + evict) are guarded by mtx_, but the slow
  // chunk decompression in loadChunk() runs UNLOCKED so a slow decode by
  // one consumer doesn't block lookups by others. If two threads miss the
  // same chunk simultaneously, both load it and the second-to-finish drops
  // its result; in exchange we never serialize decompression behind the
  // mutex.
  std::shared_ptr<const Entry> get(
      mcap::McapReader& reader, uint64_t chunk_byte_start, uint64_t chunk_byte_end, mcap::Timestamp chunk_ts_start,
      mcap::Timestamp chunk_ts_end) {
    {
      std::lock_guard lock(mtx_);
      if (auto it = map_.find(chunk_byte_start); it != map_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru_it);
        return it->second.entry;
      }
    }
    auto entry = loadChunk(reader, chunk_byte_start, chunk_byte_end, chunk_ts_start, chunk_ts_end);
    if (!entry) {
      return nullptr;
    }
    std::lock_guard lock(mtx_);
    if (auto it = map_.find(chunk_byte_start); it != map_.end()) {
      // Another thread inserted the same chunk while we were loading; use
      // theirs and drop ours so consumers share one anchor per chunk.
      lru_.splice(lru_.begin(), lru_, it->second.lru_it);
      return it->second.entry;
    }
    lru_.push_front(chunk_byte_start);
    map_.emplace(chunk_byte_start, MapValue{entry, lru_.begin()});
    total_bytes_ += entry->bytes->size();
    // Keep at least one entry so a chunk larger than the budget can still
    // be cached (it survives until the next insert displaces it).
    while (map_.size() > 1 && total_bytes_ > capacity_bytes_) {
      uint64_t victim = lru_.back();
      auto victim_it = map_.find(victim);
      total_bytes_ -= victim_it->second.entry->bytes->size();
      lru_.pop_back();
      map_.erase(victim_it);
    }
    return entry;
  }

 private:
  static std::shared_ptr<const Entry> loadChunk(
      mcap::McapReader& reader, uint64_t chunk_byte_start, uint64_t chunk_byte_end, mcap::Timestamp chunk_ts_start,
      mcap::Timestamp chunk_ts_end) {
    auto buffer = std::make_shared<std::vector<uint8_t>>();
    auto entry = std::make_shared<Entry>();
    mcap::LinearMessageView view(
        reader, chunk_byte_start, chunk_byte_end, chunk_ts_start, chunk_ts_end + 1, [](const mcap::Status&) {});
    for (const auto& v : view) {
      const auto* data = reinterpret_cast<const uint8_t*>(v.message.data);
      const size_t size = v.message.dataSize;
      const size_t offset = buffer->size();
      buffer->insert(buffer->end(), data, data + size);
      entry->index.emplace(Key{v.channel->id, v.message.logTime}, std::pair<size_t, size_t>{offset, size});
    }
    entry->bytes = std::shared_ptr<const std::vector<uint8_t>>(std::move(buffer));
    return entry;
  }

  struct MapValue {
    std::shared_ptr<const Entry> entry;
    std::list<uint64_t>::iterator lru_it;
  };

  size_t capacity_bytes_;
  std::mutex mtx_;
  std::list<uint64_t> lru_;  // front = MRU
  std::unordered_map<uint64_t, MapValue> map_;
  size_t total_bytes_ = 0;
};

// Resolve one message via the chunk cache. On hit (typical for sequential
// reads within a chunk) the returned Span points directly into the cached
// chunk buffer; the BufferAnchor keeps that buffer alive for as long as the
// host or consumer holds the PayloadView.
inline PJ::sdk::PayloadView readMessageBytesAt(
    const std::shared_ptr<mcap::McapReader>& reader, const std::shared_ptr<ChunkCache>& cache,
    uint64_t chunk_byte_start, uint64_t chunk_byte_end, mcap::Timestamp chunk_ts_start, mcap::Timestamp chunk_ts_end,
    mcap::ChannelId channel_id, mcap::Timestamp log_time) {
  auto entry = cache->get(*reader, chunk_byte_start, chunk_byte_end, chunk_ts_start, chunk_ts_end);
  if (!entry) {
    return {};
  }
  auto it = entry->index.find({channel_id, log_time});
  if (it == entry->index.end()) {
    return {};
  }
  const auto [offset, size] = it->second;
  return PJ::sdk::PayloadView{
      PJ::Span<const uint8_t>(entry->bytes->data() + offset, size),
      PJ::sdk::BufferAnchor{entry->bytes},
  };
}

// ─────────────────────────────────────────────────────────────────────────────
// McapSource plugin
// ─────────────────────────────────────────────────────────────────────────────

class McapSource : public PJ::FileSourceBase {
  // Chunk cache budget. 128 MiB comfortably holds tens of typical 4 MiB
  // chunks so a scrubbing consumer rarely re-decompresses, while staying
  // well below host memory pressure. Only the COLD path (post-import lazy
  // re-decompression) populates this cache — import-time eager fetches
  // come straight from the parallel reader's pinned chunk via a
  // non-owning weak_ptr.
  static constexpr size_t kChunkCacheCapacityBytes = 128ULL * 1024 * 1024;
  // Parallel-import in-flight decompression budget.
  static constexpr uint64_t kParallelImportBudgetBytes = 256ULL * 1024 * 1024;
  // Parallel-import worker count.
  static constexpr unsigned kParallelImportThreadCount = 4;

 public:
  PJ_borrowed_dialog_t getDialog() override {
    return PJ::borrowDialog(dialog_);
  }

  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override {
    return dialog_.saveConfig();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    if (!dialog_.loadConfig(config_json)) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (!cfg.is_discarded()) {
      if (cfg.contains("_parser_config")) {
        // Parser config forwarded by FileLoader from the embedded pj_parser_slot
        // dialog — takes precedence over everything else.
        parser_config_override_ = cfg["_parser_config"].get<std::string>();
      } else {
        // Migration: old configs had use_timestamp / timestamp_field_name directly
        // in the MCAP dialog config. Synthesize a parser config from those keys so
        // existing saved sessions keep working after the controls moved to
        // JsonParserDialog.
        bool use_ts = cfg.value("use_timestamp", false);
        std::string ts_field = cfg.value("timestamp_field_name", std::string{});
        if (use_ts) {
          nlohmann::json migrated;
          migrated["use_embedded_timestamp"] = true;
          migrated["timestamp_field_name"] = ts_field;
          parser_config_override_ = migrated.dump();
        } else {
          parser_config_override_.clear();
        }
      }
    } else {
      parser_config_override_.clear();
    }
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    if (dialog_.filepath().empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }

    // Persist the reader and chunk cache as shared_ptr members of the source
    // so the lambdas produced inside the message loop keep them alive past
    // the end of importData() until the last consumer-side pull is satisfied.
    // The cache holds up to kChunkCacheCapacityBytes worth of decompressed
    // chunks; on hit the fetcher returns a zero-copy Span anchored on the
    // cached buffer.
    reader_keeper_ = std::make_shared<mcap::McapReader>();
    chunk_cache_ = std::make_shared<ChunkCache>(kChunkCacheCapacityBytes);
    auto& reader = *reader_keeper_;
    auto status = reader.open(dialog_.filepath());
    if (!status.ok()) {
      return PJ::unexpected(std::string("cannot open MCAP file: ") + status.message);
    }

    // --- Read summary (schemas, channels, statistics) ---
    McapSummaryInfo summary;
    bool used_selective_summary = false;
    status = readSelectiveSummary(*reader.dataSource(), summary);
    if (status.ok()) {
      used_selective_summary = true;
    } else {
      status = reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
      if (!status.ok()) {
        runtimeHost().showError(
            "Can't open summary of the file",
            "Code: " + std::to_string(static_cast<int>(status.code)) + "\nMessage: " + status.message);
        reader_keeper_.reset();  // bail out: drop the keeper so the reader closes.
        chunk_cache_.reset();
        return PJ::unexpected(std::string("cannot read MCAP summary: ") + status.message);
      }
      populateSummaryFromReader(reader, summary);
    }

    // readSelectiveSummary deliberately skips ChunkIndex parsing for a
    // faster initial open, but the random-access FetchMessageData below depends on
    // `reader.chunkIndexes()` to know each chunk's byte range and time
    // bounds. If we took the selective path, do an additional
    // NoFallbackScan summary read solely to populate that data. Strictly
    // necessary for byte-bounded LinearMessageView.
    if (used_selective_summary && reader.chunkIndexes().empty()) {
      auto idx_status = reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
      if (!idx_status.ok()) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning,
            std::string("chunk index unavailable: ") + idx_status.message + "; random-access pulls will be slow.");
      }
    }

    uint64_t total_messages = 0;
    if (summary.statistics) {
      total_messages = summary.statistics->messageCount;
    }
    (void)runtimeHost().progressStart("Importing MCAP", total_messages, true);

    // --- Build parser config: prefer embedded parser dialog config when
    //     available (set via the pj_parser_slot mechanism), otherwise fall
    //     back to building it from the individual dialog accessors.
    std::string parser_config_str;
    // parser_config comes entirely from the embedded parser dialog (pj_parser_slot).
    // When no override is set (first run / old config), use an empty config so
    // the parser falls back to its own defaults.
    parser_config_str = parser_config_override_;

    // --- Ensure parser bindings for selected channels ---
    const auto& selected = dialog_.selectedTopics();
    std::unordered_map<mcap::ChannelId, PJ::ParserBindingHandle> bindings;
    std::vector<std::string> binding_errors;

    for (const auto& [channel_id, channel_ptr] : summary.channels) {
      // Filter by dialog selection
      if (selected.find(channel_ptr->topic) == selected.end()) {
        continue;
      }

      auto schema_it = summary.schemas.find(channel_ptr->schemaId);
      if (schema_it == summary.schemas.end()) {
        continue;
      }
      const auto& schema = schema_it->second;

      PJ::Span<const uint8_t> schema_bytes{reinterpret_cast<const uint8_t*>(schema->data.data()), schema->data.size()};

      std::string_view encoding = channel_ptr->messageEncoding;
      if (encoding.empty()) {
        encoding = schema->encoding;
      }

      PJ::ParserBindingRequest request{
          .topic_name = channel_ptr->topic,
          .parser_encoding = encoding,
          .type_name = schema->name,
          .schema = schema_bytes,
          .parser_config_json = parser_config_str,
      };

      // Bind the parser. The host runtime, internally, also asks the parser
      // about its schema classification (classifySchema) and — when the
      // parser declares a builtin object type != kNone — registers the
      // matching ObjectTopic in the ObjectStore on the source's behalf,
      // associated with this binding. The DataSource never inspects
      // schema->name nor mentions object_type anywhere.
      auto handle = runtimeHost().ensureParserBinding(request);
      if (!handle) {
        binding_errors.push_back(channel_ptr->topic + " (encoding: " + std::string(encoding) + "): " + handle.error());
        continue;
      }
      bindings.emplace(channel_id, *handle);
    }

    if (bindings.empty()) {
      std::string msg = "No channels could be bound to parsers:\n";
      for (const auto& e : binding_errors) {
        msg += "  - " + e + "\n";
      }
      runtimeHost().showError("Parser Error", msg);
      reader_keeper_.reset();
      chunk_cache_.reset();
      return PJ::unexpected(msg);
    }

    if (!binding_errors.empty()) {
      std::string msg = std::to_string(binding_errors.size()) + " channel(s) skipped (no parser):\n";
      for (const auto& e : binding_errors) {
        msg += "  - " + e + "\n";
      }
      runtimeHost().showWarning("Parser Error", msg);
    }

    // --- Iterate messages via the parallel reader ---
    // The fork's parallel reader decompresses chunks on a worker pool ahead
    // of the merge frontier. Each ReadyChunk is held only while at least one
    // message from it is being emitted; chunks are evicted by a byte budget
    // when no longer referenced.
    //
    // For each message we capture a NON-OWNING weak_ptr to the chunk in the
    // fetcher closure. During import, the host's eager-policy invocation of
    // the fetcher races with the parallel reader's normal pinning: lock()
    // succeeds, we return a Span into the chunk's bytes. The parser consumes
    // them synchronously and the locked shared_ptr is released. Memory stays
    // bounded by the parallel reader's own byte budget.
    //
    // After import the parallel reader is destroyed; weak_ptr.lock() returns
    // null on subsequent invocations and the closure falls back to the cold
    // path — readMessageBytesAt with the long-lived reader_keeper_, which
    // re-decompresses the chunk on demand and stores it in the LRU
    // ChunkCache. True lazy.
    auto on_problem = [this](const mcap::Status& problem) {
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, problem.message);
    };

    uint64_t msg_count = 0;
    const bool use_log_time = dialog_.useMcapLogTime();

    // Cold-path metadata: byte range + time bounds keyed by chunk file offset,
    // used by the bounded LinearMessageView inside ChunkCache::loadChunk.
    struct ChunkMeta {
      uint64_t length;
      mcap::Timestamp ts_start;
      mcap::Timestamp ts_end;
    };
    std::unordered_map<uint64_t, ChunkMeta> chunk_meta;
    for (const auto& ci : reader.chunkIndexes()) {
      chunk_meta.emplace(
          ci.chunkStartOffset,
          ChunkMeta{.length = ci.chunkLength, .ts_start = ci.messageStartTime, .ts_end = ci.messageEndTime});
    }

    // mmap-backed source for the parallel reader; local to importData so its
    // lifetime is bounded by the inner block below (and the parallel reader
    // it backs).
    mcap::MmapReader mmap_reader;
    if (auto status_mmap = mmap_reader.open(dialog_.filepath()); !status_mmap.ok()) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kError, std::string("cannot mmap MCAP file: ") + status_mmap.message);
      return PJ::unexpected(std::string("cannot mmap MCAP file: ") + status_mmap.message);
    }

    {
      mcap::ParallelReader parallel_reader;
      if (auto pstatus = parallel_reader.open(mmap_reader); !pstatus.ok()) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kWarning, std::string("parallel reader open failed: ") + pstatus.message);
      }

      mcap::ParallelReadOptions parallel_opts;
      parallel_opts.read.readOrder = mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;
      parallel_opts.maxBytesInFlight = kParallelImportBudgetBytes;
      parallel_opts.threadCount = kParallelImportThreadCount;
      parallel_opts.read.topicFilter = [selected_topics = dialog_.selectedTopics()](std::string_view topic) {
        return selected_topics.find(std::string(topic)) != selected_topics.end();
      };

      auto messages = parallel_reader.readMessages(on_problem, parallel_opts);
      try {
        // Explicit iterator so we can call currentChunk() per message.
        for (auto it = messages.begin(); it != messages.end(); ++it) {
          const auto& msg_view = *it;
          if (msg_view.channel == nullptr || msg_view.message.data == nullptr) {
            continue;
          }
          auto binding_it = bindings.find(msg_view.channel->id);
          if (binding_it == bindings.end()) {
            continue;
          }

          const PJ::Timestamp timestamp_ns =
              static_cast<PJ::Timestamp>(use_log_time ? msg_view.message.logTime : msg_view.message.publishTime);

          // RecordOffset semantics in the parallel reader fork:
          //   .offset       = within-chunk position of the message record
          //   .chunkOffset  = chunk record's FILE offset (= ChunkIndex::chunkStartOffset)
          const uint64_t chunk_off = msg_view.messageOffset.chunkOffset.value_or(msg_view.messageOffset.offset);
          uint64_t chunk_len = 0;
          mcap::Timestamp chunk_ts_start = msg_view.message.logTime;
          mcap::Timestamp chunk_ts_end = msg_view.message.logTime;
          if (auto mit = chunk_meta.find(chunk_off); mit != chunk_meta.end()) {
            chunk_len = mit->second.length;
            chunk_ts_start = mit->second.ts_start;
            chunk_ts_end = mit->second.ts_end;
          }

          // Compute the message data's byte offset within the chunk's
          // decompressed buffer. msg_view.message.data points into
          // ReadyChunk::bytes (a vector<std::byte>); the offset is stable
          // and reusable when the closure later locks the weak_ptr.
          auto chunk_handle = it.currentChunk();
          const size_t data_size = msg_view.message.dataSize;
          size_t data_offset = 0;
          bool hot_path_eligible = false;
          if (auto chunk = chunk_handle.lock()) {
            const auto* chunk_base = reinterpret_cast<const uint8_t*>(chunk->bytes.data());
            const auto* msg_data = reinterpret_cast<const uint8_t*>(msg_view.message.data);
            if (msg_data >= chunk_base && msg_data + data_size <= chunk_base + chunk->bytes.size()) {
              data_offset = static_cast<size_t>(msg_data - chunk_base);
              hot_path_eligible = true;
            }
          }
          if (!hot_path_eligible) {
            chunk_handle = {};  // disable the hot path; fetcher will go cold
          }

          // Fetcher closure: weak_ptr is NON-OWNING. If the host stores this
          // for a later lazy pull, the parallel reader is gone by then,
          // lock() returns null, and we fall through to readMessageBytesAt
          // (which lazily decompresses + caches via ChunkCache).
          auto push_status = runtimeHost().pushMessage(
              binding_it->second, timestamp_ns,
              [chunk_handle, data_offset, data_size, reader = reader_keeper_, cache = chunk_cache_, chunk_off,
               chunk_len, chunk_ts_start, chunk_ts_end, ch = msg_view.channel->id,
               lt = msg_view.message.logTime]() -> PJ::sdk::PayloadView {
                // Hot path: chunk still pinned by parallel reader's worker
                // pool. Return a Span into its bytes with the locked
                // shared_ptr as anchor. Anchor is released when the host
                // finishes consuming this PayloadView.
                if (auto chunk = chunk_handle.lock()) {
                  if (data_offset + data_size <= chunk->bytes.size()) {
                    const auto* base = reinterpret_cast<const uint8_t*>(chunk->bytes.data());
                    return PJ::sdk::PayloadView{
                        PJ::Span<const uint8_t>(base + data_offset, data_size),
                        PJ::sdk::BufferAnchor{std::move(chunk)},
                    };
                  }
                }
                // Cold path: chunk evicted or parallel reader gone. Lazy
                // re-decompress via ChunkCache.
                return readMessageBytesAt(
                    reader, cache, chunk_off, chunk_off + chunk_len, chunk_ts_start, chunk_ts_end, ch, lt);
              });
          if (!push_status) {
            runtimeHost().reportMessage(
                PJ::DataSourceMessageLevel::kWarning,
                std::string("push failed on '") + msg_view.channel->topic + "': " + push_status.error());
          }

          ++msg_count;
          if (msg_count % 1000 == 0) {
            if (!runtimeHost().progressUpdate(msg_count)) {
              break;
            }
            if (runtimeHost().isStopRequested()) {
              break;
            }
          }
        }
      } catch (const std::exception& e) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kError,
            std::string("MCAP import aborted: ") + e.what() + " (loaded " + std::to_string(msg_count) + " messages)");
      } catch (...) {
        runtimeHost().reportMessage(
            PJ::DataSourceMessageLevel::kError,
            std::string("MCAP import aborted on unknown error (loaded ") + std::to_string(msg_count) + " messages)");
      }
    }  // ~messages, ~parallel_reader run here, before mmap_reader destructs at scope end.

    // Intentionally NOT closing reader_keeper_: it stays alive past
    // importData() so the captured fetcher closures can re-decompress on
    // demand via the cold path. It closes when the source destructs.
    return PJ::okStatus();
  }

 private:
  McapDialog dialog_;
  // Parser config from the embedded parser dialog (pj_parser_slot). Set by
  // loadConfig() when FileLoader embeds it under "_parser_config". When
  // non-empty, takes precedence over per-field accessors in McapDialog.
  std::string parser_config_override_;
  // Keeps the open mcap reader alive past importData() so the deferred
  // FetchMessageData callables handed to runtimeHost().pushMessage() can read messages on
  // demand. Reset on bail-out paths or destroyed with the source.
  std::shared_ptr<mcap::McapReader> reader_keeper_;
  // LRU cache of decompressed chunks shared with every fetcher closure.
  // First pull into a chunk loads it; subsequent pulls are zero-copy spans
  // anchored on the cached buffer.
  std::shared_ptr<ChunkCache> chunk_cache_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(McapSource, kMcapManifest)

PJ_DIALOG_PLUGIN(McapDialog)
