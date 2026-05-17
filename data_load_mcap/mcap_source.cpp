#include <pj_base/builtin/BuiltinObject.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>

#define MCAP_IMPLEMENTATION
#include <cstdint>
#include <memory>
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

namespace {

using McapSummaryInfo = PJ::McapHelpers::McapSummaryInfo;
using PJ::McapHelpers::populateSummaryFromReader;
using PJ::McapHelpers::readSelectiveSummary;

// (Removed: the previous hardcoded blobBearingSchemas() set. Whether a topic
// produces a blob is now declared by the bound MessageParser via
// SchemaClassification — see the call to runtimeHost().classificationOf()
// inside importData(). The DataSource no longer knows ROS schema names.)

// Read one message's bytes via TRUE random access.
//
// Hands LinearMessageView a byte range bounded exactly to the containing
// chunk (`chunk_offset, chunk_offset + chunk_length`) so mcap loads and
// decompresses that single chunk directly — no linear scan from the
// data section start. The standard `readMessages(start_ts, end_ts)` API
// would cost O(chunks_before_target) per pull because it walks the data
// section sequentially until it finds the first chunk overlapping
// `start_ts`. With files like Air_Frying_Potatoes_d63fbbf5.mcap (8374
// chunks), that linear scan dominates: perf shows 90% of CPU in the
// kernel read path triggered by `mcap::FileReader::read`, and the
// asymmetry (fast early frames, slow late frames) traces directly to
// "more chunks to skip the further you scrub". Byte-bounded
// LinearMessageView is O(1) regardless of where the chunk sits.
//
// No chunk cache — every pull decompresses its chunk once and returns
// the message bytes copied into a heap-held vector that serves as the
// PayloadView anchor. Adding an LRU is a separate concern (helps when
// the same chunk is hit many times in quick succession); the random-
// access fix on its own already removes the scaling bottleneck.
inline PJ::sdk::PayloadView readMessageBytesAt(
    const std::shared_ptr<mcap::McapReader>& reader, uint64_t chunk_byte_start, uint64_t chunk_byte_end,
    mcap::Timestamp chunk_ts_start, mcap::Timestamp chunk_ts_end, mcap::ChannelId channel_id,
    mcap::Timestamp log_time) {
  mcap::LinearMessageView view(
      *reader, chunk_byte_start, chunk_byte_end, chunk_ts_start, chunk_ts_end + 1, [](const mcap::Status&) {});

  for (const auto& v : view) {
    if (v.channel->id == channel_id && v.message.logTime == log_time) {
      auto owned = std::make_shared<std::vector<uint8_t>>(
          reinterpret_cast<const uint8_t*>(v.message.data),
          reinterpret_cast<const uint8_t*>(v.message.data) + v.message.dataSize);
      return PJ::sdk::PayloadView{
          PJ::Span<const uint8_t>(owned->data(), owned->size()),
          owned,
      };
    }
  }
  return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// McapSource plugin
// ─────────────────────────────────────────────────────────────────────────────

class McapSource : public PJ::FileSourceBase {
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
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    if (dialog_.filepath().empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }

    // Persist the reader as a shared_ptr member of the source so the lambdas
    // produced inside the message loop (captured by reader_keeper_) can keep
    // it alive past the end of importData() until the last consumer-side
    // pull is satisfied. Closing now would break lazy modes; the destructor
    // releases reader_keeper_ when the source itself goes away.
    reader_keeper_ = std::make_shared<mcap::McapReader>();
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

    // --- Build parser config JSON from dialog settings ---
    nlohmann::json parser_config;
    parser_config["max_array_size"] = dialog_.maxArraySize();
    parser_config["use_embedded_timestamp"] = dialog_.useTimestamp();
    parser_config["clamp_large_arrays"] = dialog_.clampLargeArrays();
    std::string parser_config_str = parser_config.dump();

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
      return PJ::unexpected(msg);
    }

    if (!binding_errors.empty()) {
      std::string msg = std::to_string(binding_errors.size()) + " channel(s) skipped (no parser):\n";
      for (const auto& e : binding_errors) {
        msg += "  - " + e + "\n";
      }
      runtimeHost().showWarning("Parser Error", msg);
    }

    // --- Iterate messages and push raw bytes ---
    auto on_problem = [this](const mcap::Status& problem) {
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, problem.message);
    };

    auto create_message_view = [&]() -> mcap::LinearMessageView {
      if (used_selective_summary) {
        auto [data_start, data_end_unused] = reader.byteRange(0);
        (void)data_end_unused;
        return mcap::LinearMessageView(reader, data_start, summary.summary_start, 0, mcap::MaxTime, on_problem);
      }
      return reader.readMessages(on_problem);
    };

    auto messages = create_message_view();
    uint64_t msg_count = 0;
    bool use_log_time = dialog_.useMcapLogTime();

    // Build a chunk_offset → (length, start_time, end_time) index from the
    // summary. The FetchMessageData closure captures the containing chunk's byte
    // range AND time bounds so the cache loader can construct a
    // byte-bounded LinearMessageView — O(1) random access instead of
    // O(N_chunks_before_target).
    struct ChunkBounds {
      uint64_t length;
      mcap::Timestamp ts_start;
      mcap::Timestamp ts_end;
    };
    std::unordered_map<uint64_t, ChunkBounds> chunk_index;
    for (const auto& ci : reader.chunkIndexes()) {
      chunk_index.emplace(
          ci.chunkStartOffset,
          ChunkBounds{.length = ci.chunkLength, .ts_start = ci.messageStartTime, .ts_end = ci.messageEndTime});
    }

    for (const auto& msg_view : messages) {
      auto binding_it = bindings.find(msg_view.channel->id);
      if (binding_it == bindings.end()) {
        continue;
      }

      PJ::Timestamp timestamp_ns =
          static_cast<PJ::Timestamp>(use_log_time ? msg_view.message.logTime : msg_view.message.publishTime);

      // Lookup the containing chunk in the summary-built index.
      //
      // Note on RecordOffset semantics inside LinearMessageView: for
      // messages stored inside a chunk, `messageOffset.offset` is the
      // **file offset of the containing chunk record** (matching
      // ChunkIndex::chunkStartOffset), and `messageOffset.chunkOffset`
      // is the byte position of the message WITHIN the decompressed
      // chunk body — the field names are misleading. We use `offset`
      // because that's what indexes into ChunkIndex.
      const uint64_t chunk_off = msg_view.messageOffset.offset;
      uint64_t chunk_len = 0;
      mcap::Timestamp chunk_ts_start = msg_view.message.logTime;
      mcap::Timestamp chunk_ts_end = msg_view.message.logTime;
      if (auto rit = chunk_index.find(chunk_off); rit != chunk_index.end()) {
        chunk_len = rit->second.length;
        chunk_ts_start = rit->second.ts_start;
        chunk_ts_end = rit->second.ts_end;
      }

      // Single uniform call per message. The DataSource hands the host a
      // FetchMessageData callable (produces the payload bytes when invoked)
      // and stays out of any further routing. The host applies the
      // configured ObjectIngestPolicy (kPureLazy / kLazyObjectsEagerScalars / kEager)
      // to decide whether to invoke the callable now (parse scalars and/or
      // materialize the object) or only register it for later pulls. The
      // DataSource never knows which mode is active.
      //
      // The lambda captures the chunk's byte range + time bounds + the
      // open mcap reader. On pull, readMessageBytesAt() builds a
      // byte-bounded LinearMessageView to load + decompress exactly that
      // chunk and slice the target message.
      auto push_status = runtimeHost().pushMessage(
          binding_it->second, timestamp_ns,
          [reader = reader_keeper_, chunk_off, chunk_len, chunk_ts_start, chunk_ts_end, ch = msg_view.channel->id,
           lt = msg_view.message.logTime]() {
            return readMessageBytesAt(reader, chunk_off, chunk_off + chunk_len, chunk_ts_start, chunk_ts_end, ch, lt);
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

    // Intentionally NOT closing the reader here. reader_keeper_ stays alive
    // as a member of the source so the lambdas the host captured during the
    // message loop can still hit the file when the configured ObjectIngestPolicy
    // resolves them on pull. The reader closes automatically when the source
    // itself is destroyed.
    return PJ::okStatus();
  }

 private:
  McapDialog dialog_;
  // Keeps the open mcap reader alive past importData() so the deferred
  // FetchMessageData callables handed to runtimeHost().pushMessage() can read messages on
  // demand. Reset on bail-out paths or destroyed with the source.
  std::shared_ptr<mcap::McapReader> reader_keeper_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(McapSource, kMcapManifest)

PJ_DIALOG_PLUGIN(McapDialog)
