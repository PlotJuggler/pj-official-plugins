#include <pj_base/sdk/canonical_object.hpp>
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

// Read the bytes of a single mcap message identified by its RecordOffset.
// Used by the deferred fetcher captured in the per-message lambda below.
//
// TODO(rfc): direct seek+read using the open reader's data source.
//   - For records outside any chunk (rare for messages):
//       reader->dataSource()->read(offset.offset + record_header_bytes, ...)
//   - For records inside a chunk (typical):
//       locate chunk via offset.chunkOffset, decompress (lz4/zstd if applies),
//       copy bytes at the relative offset. A small LRU of decompressed chunks
//       would amortize scrubbing across the same chunk and let consumers
//       share ownership of the chunk via the PayloadView anchor — zero-copy
//       all the way to parseObject.
//
// Until the helper above lands, fall back to the range iterator: ask the
// reader for messages in [log_time, log_time+1) and pick the matching one.
// Correct, sub-optimal — the reader will re-decompress the chunk on each
// call when scrubbing across chunks. The single copy from mcap's iterator
// bytes into a heap-held vector serves as the PayloadView anchor; from
// there the parser's parse<X>() builds Span+anchor over the SAME vector,
// so zero-copy is preserved through the rest of the pipeline. Production
// LRU path replaces the per-call copy with chunk-shared ownership.
inline PJ::sdk::PayloadView readMessageBytesAt(
    const std::shared_ptr<mcap::McapReader>& reader, mcap::RecordOffset /*offset*/, mcap::ChannelId channel_id,
    mcap::Timestamp log_time) {
  auto noop_problem = [](const mcap::Status&) {};
  for (const auto& v : reader->readMessages(noop_problem, log_time, log_time + 1)) {
    if (v.channel->id == channel_id) {
      auto owned = std::make_shared<std::vector<uint8_t>>(
          reinterpret_cast<const uint8_t*>(v.message.data),
          reinterpret_cast<const uint8_t*>(v.message.data) + v.message.dataSize);
      PJ::Span<const uint8_t> view(owned->data(), owned->size());
      return PJ::sdk::PayloadView{view, owned};
    }
  }
  return PJ::sdk::PayloadView{};
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
      // parser declares a canonical-object kind != kNone — registers the
      // matching ObjectTopic in the ObjectStore on the source's behalf,
      // associated with this binding. The DataSource never inspects
      // schema->name nor mentions object_kind anywhere.
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

    for (const auto& msg_view : messages) {
      auto binding_it = bindings.find(msg_view.channel->id);
      if (binding_it == bindings.end()) {
        continue;
      }

      PJ::Timestamp timestamp_ns =
          static_cast<PJ::Timestamp>(use_log_time ? msg_view.message.logTime : msg_view.message.publishTime);

      // Single uniform call per message. The DataSource hands the host a
      // fetcher (callable that produces the payload bytes when invoked)
      // and stays out of any further routing. The host applies the
      // configured ObjectIngestPolicy (kPureLazy / kLazyObjectsEagerScalars / kEager)
      // to decide whether to invoke the fetcher now (parse scalars and/or
      // materialize the object) or only register it for later pulls. The
      // DataSource never knows which mode is active.
      //
      // The lambda captures the message's RecordOffset (file coordinates)
      // and the open mcap reader (kept alive by the source for the whole
      // session). When the host invokes it — at ingest, on pull, or never
      // — readMessageBytesAt() materializes the bytes for that one
      // message. Bytes never persist past the lambda's return value
      // lifetime.
      auto push_status = runtimeHost().pushMessage(
          binding_it->second, timestamp_ns,
          [reader = reader_keeper_, off = msg_view.messageOffset, ch = msg_view.channel->id,
           lt = msg_view.message.logTime]() { return readMessageBytesAt(reader, off, ch, lt); });
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
  // fetchers handed to runtimeHost().pushMessage() can read messages on
  // demand. Reset on bail-out paths or destroyed with the source.
  std::shared_ptr<mcap::McapReader> reader_keeper_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(McapSource, kMcapManifest)

PJ_DIALOG_PLUGIN(McapDialog)
