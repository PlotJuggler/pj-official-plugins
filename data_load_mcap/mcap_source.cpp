#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>

#define MCAP_IMPLEMENTATION
#include <algorithm>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mcap_dialog.hpp"
#include "mcap_helpers.hpp"
#include "mcap_manifest.hpp"

// Vendored parallel-reader fork (contrib/mcap/). Pulled in after reader.hpp
// so MCAP_IMPLEMENTATION already lit up reader.inl; parallel reader is fully
// inline and stacks on top. message_byte_store.hpp adds the optional
// deferred-byte (hot/cold lazy) layer on top of the parallel reader.
#include <mcap/message_byte_store.hpp>  // NOLINT(build/include_order)
#include <mcap/parallel_reader.hpp>     // NOLINT(build/include_order)

namespace {

// Hardware-derived worker count. Floor at 2 so we still get parallelism on
// virtualized hosts that report 1 core; cap at 8 to avoid oversubscription on
// many-core machines where the byte-budget and consumer drain rate become the
// real limits anyway.
inline unsigned parallelImportThreadCount() {
  const unsigned hw = std::thread::hardware_concurrency();
  return std::min(8u, std::max(2u, hw));
}

// ─────────────────────────────────────────────────────────────────────────────
// McapSource plugin
// ─────────────────────────────────────────────────────────────────────────────

class McapSource : public PJ::FileSourceBase {
  // Cold-path chunk cache budget, handed to the MessageByteStore. 128 MiB
  // comfortably holds tens of typical 4 MiB chunks so a scrubbing consumer
  // rarely re-decompresses, while staying well below host memory pressure.
  // Only post-import lazy pulls populate it — eager import-time fetches come
  // straight from the parallel reader's pinned chunk (zero-copy, no cache).
  static constexpr size_t kChunkCacheCapacityBytes = 128ULL * 1024 * 1024;
  // Parallel-import in-flight decompression budget.
  static constexpr uint64_t kParallelImportBudgetBytes = 256ULL * 1024 * 1024;
  // Progress / log throttling.
  static constexpr uint64_t kProgressUpdateInterval = 1000;
  // Push-failure threshold + log throttling. A handful of transient push
  // failures is acceptable (parser binding temporarily backpressured); a
  // sustained run means something structural is broken and the import
  // should abort rather than spam the message panel.
  static constexpr uint64_t kMaxConsecutivePushFailures = 100;
  static constexpr uint64_t kPushFailureLogInterval = 25;

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

    // open(path) gives the reader an internally-owned ConcurrentFileReader
    // (positioned pread/ReadFile, concurrent-safe). Declared before `messages`
    // below so it outlives the message view.
    mcap::ParallelReader parallel_reader;
    if (auto st = parallel_reader.open(dialog_.filepath()); !st.ok()) {
      const std::string msg = std::string("cannot open MCAP file: ") + st.message;
      runtimeHost().showError("MCAP import failed", msg);
      return PJ::unexpected(msg);
    }
    // parallel_reader.open() runs an AllowFallbackScan summary, so channels,
    // schemas, statistics and chunk indexes are all available now — no separate
    // reader or summary parse is needed for binding setup or the cold path.

    uint64_t total_messages = 0;
    if (parallel_reader.statistics()) {
      total_messages = parallel_reader.statistics()->messageCount;
    }
    (void)runtimeHost().progressStart("Importing MCAP", total_messages, true);

    // Parser config comes entirely from the embedded parser dialog
    // (pj_parser_slot). Empty when no override is set (first run / old config),
    // so the parser falls back to its own defaults.
    nlohmann::json parser_config = nlohmann::json::object();
    if (!parser_config_override_.empty()) {
      auto parsed_config = nlohmann::json::parse(parser_config_override_, nullptr, false);
      if (parsed_config.is_object()) {
        parser_config = std::move(parsed_config);
      }
    }

    // --- Ensure parser bindings for selected channels ---
    const auto& selected = dialog_.selectedTopics();
    const auto channels = parallel_reader.channels();
    const auto schemas = parallel_reader.schemas();
    std::unordered_map<mcap::ChannelId, PJ::ParserBindingHandle> bindings;
    std::vector<std::string> binding_errors;

    for (const auto& [channel_id, channel_ptr] : channels) {
      // Filter by dialog selection.
      if (selected.find(channel_ptr->topic) == selected.end()) {
        continue;
      }

      auto schema_it = schemas.find(channel_ptr->schemaId);
      if (schema_it == schemas.end()) {
        continue;
      }
      const auto& schema = schema_it->second;

      PJ::Span<const uint8_t> schema_bytes{reinterpret_cast<const uint8_t*>(schema->data.data()), schema->data.size()};

      std::string parser_encoding = channel_ptr->messageEncoding;
      if (parser_encoding.empty() || (parser_encoding == "cdr" && !schema->encoding.empty())) {
        parser_encoding = schema->encoding;
      }

      auto channel_parser_config = parser_config;
      const bool use_ros1_serialization = channel_ptr->messageEncoding == "ros1" || parser_encoding == "ros1" ||
                                          parser_encoding == "ros1msg" || schema->encoding == "ros1msg";
      channel_parser_config["serialization"] = use_ros1_serialization ? "ros1" : "cdr";
      channel_parser_config["schema_encoding"] = parser_encoding;
      const std::string parser_config_str = channel_parser_config.dump();

      PJ::ParserBindingRequest request{
          .topic_name = channel_ptr->topic,
          .parser_encoding = parser_encoding,
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
        binding_errors.push_back(channel_ptr->topic + " (encoding: " + parser_encoding + "): " + handle.error());
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
      return PJ::unexpected(msg);
    }

    if (!binding_errors.empty()) {
      std::string msg = std::to_string(binding_errors.size()) + " channel(s) skipped (no parser):\n";
      for (const auto& e : binding_errors) {
        msg += "  - " + e + "\n";
      }
      runtimeHost().showWarning("Parser Error", msg);
    }

    // Cold-path byte store, seeded from the parallel reader's chunk index (no
    // extra summary parse). Member, so it — and the fetchers that share its
    // cold state — outlive importData(): post-import lazy pulls re-decompress on
    // demand through it. The FileReader it needs is opened lazily on the first
    // cold miss, so fully-eager imports never reopen the file.
    byte_store_.init(
        dialog_.filepath(), parallel_reader.chunkIndexes(), {.cacheCapacityBytes = kChunkCacheCapacityBytes},
        [this](const mcap::Status& problem) {
          runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, problem.message);
        });

    // --- Iterate messages via the parallel reader ---
    // The fork decompresses chunks on a worker pool ahead of the merge frontier
    // and keeps each chunk pinned only while a message from it is being emitted.
    // For every message we hand the host a MessageByteFetcher:
    //   * invoked eagerly/synchronously during this loop, it returns a zero-copy
    //     view into the still-pinned, worker-decompressed bytes (no re-decompress);
    //   * invoked lazily after import, the pinned chunk is gone, so it falls back
    //     to the byte store's bounded cold re-decompression. True lazy.
    auto on_problem = [this](const mcap::Status& problem) {
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, problem.message);
    };

    uint64_t msg_count = 0;
    uint64_t consecutive_push_failures = 0;
    const bool use_log_time = dialog_.useTimestamp();

    // Authoritative recording window (log-time envelope) from the MCAP summary,
    // already read by parallel_reader.open(). Header (publishTime) stamps that
    // fall outside it come from latched / long-running publishers — static TF,
    // robot_description, a map computed hours or days earlier — and would drag
    // the playback range off the actual recording. For those we fall back to the
    // message's own logTime (see the per-message timestamp below). If the summary
    // has no statistics the window stays fully open and behaviour is unchanged.
    uint64_t log_window_min = 0;
    uint64_t log_window_max = UINT64_MAX;
    if (parallel_reader.statistics()) {
      log_window_min = parallel_reader.statistics()->messageStartTime;
      log_window_max = parallel_reader.statistics()->messageEndTime;
    }

    mcap::ParallelReadOptions parallel_opts;
    parallel_opts.read.readOrder = mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;
    parallel_opts.maxBytesInFlight = kParallelImportBudgetBytes;
    parallel_opts.threadCount = parallelImportThreadCount();
    parallel_opts.read.topicFilter = [selected_topics = dialog_.selectedTopics()](std::string_view topic) {
      return selected_topics.find(std::string(topic)) != selected_topics.end();
    };

    // Track terminal failure so we can return PJ::unexpected after the message
    // view and parallel reader cleanly destruct. A failed push run or a thrown
    // exception must propagate to FileLoader as a load failure, not get silently
    // downgraded to okStatus() with a warning toast.
    std::optional<std::string> import_failure;
    {
      auto messages = parallel_reader.readMessages(on_problem, parallel_opts);
      try {
        // Explicit iterator so makeFetcher() can capture the current message's
        // pinned-buffer handle (it.currentBuffer()).
        for (auto it = messages.begin(); it != messages.end(); ++it) {
          const auto& mv = *it;
          if (mv.channel == nullptr || mv.message.data == nullptr) {
            continue;
          }
          auto binding_it = bindings.find(mv.channel->id);
          if (binding_it == bindings.end()) {
            continue;
          }

          // Window fallback: trust the header (publishTime) only when it lands
          // inside the recording's log-time window. A header outside it comes from
          // a latched / long-running publisher (static TF, robot_description, a map
          // computed hours/days earlier); pin those to the recording's first
          // timestamp (log_window_min) so the data is valid from the very start of
          // playback rather than at its own arrival offset. useTimestamp() forces
          // the message's own logTime unconditionally.
          const uint64_t pub_time = mv.message.publishTime;
          const bool header_in_window = pub_time >= log_window_min && pub_time <= log_window_max;
          PJ::Timestamp timestamp_ns;
          if (use_log_time) {
            timestamp_ns = static_cast<PJ::Timestamp>(mv.message.logTime);
          } else if (header_in_window) {
            timestamp_ns = static_cast<PJ::Timestamp>(pub_time);
          } else {
            timestamp_ns = static_cast<PJ::Timestamp>(log_window_min);
          }

          // The fetcher captures the hot (pinned-chunk) handle + cold locator
          // now; the lambda just adapts the std-only ByteView to PayloadView
          // (BufferAnchor == std::shared_ptr<const void>, so anchor passes
          // through unchanged).
          auto push_status = runtimeHost().pushMessage(
              binding_it->second, timestamp_ns, [fetcher = byte_store_.makeFetcher(it, mv)]() {
                mcap::ByteView v = fetcher();
                return PJ::sdk::PayloadView{
                    PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(v.data), v.size),
                    v.anchor,
                };
              });
          if (!push_status) {
            ++consecutive_push_failures;
            if (consecutive_push_failures % kPushFailureLogInterval == 1) {
              runtimeHost().reportMessage(
                  PJ::DataSourceMessageLevel::kWarning,
                  std::string("push failed on '") + mv.channel->topic + "': " + push_status.error() +
                      " (consecutive failures: " + std::to_string(consecutive_push_failures) + ")");
            }
            if (consecutive_push_failures >= kMaxConsecutivePushFailures) {
              import_failure = "MCAP import aborted: " + std::to_string(consecutive_push_failures) +
                               " consecutive push failures (loaded " + std::to_string(msg_count) + " messages)";
              runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kError, *import_failure);
              break;
            }
          } else {
            consecutive_push_failures = 0;
          }

          ++msg_count;
          if (msg_count % kProgressUpdateInterval == 0) {
            if (!runtimeHost().progressUpdate(msg_count)) {
              break;
            }
            if (runtimeHost().isStopRequested()) {
              break;
            }
          }
        }
      } catch (const std::exception& e) {
        const std::string msg =
            std::string("MCAP import aborted: ") + e.what() + " (loaded " + std::to_string(msg_count) + " messages)";
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kError, msg);
        import_failure = msg;
      } catch (...) {
        const std::string msg =
            "MCAP import aborted on unknown error (loaded " + std::to_string(msg_count) + " messages)";
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kError, msg);
        import_failure = msg;
      }
    }  // ~messages runs here, before ~parallel_reader (and its owned source).

    if (import_failure) {
      return PJ::unexpected(*import_failure);
    }
    return PJ::okStatus();
  }

 private:
  McapDialog dialog_;
  // Parser config from the embedded parser dialog (pj_parser_slot). Set by
  // loadConfig() when FileLoader embeds it under "_parser_config". When
  // non-empty, takes precedence over per-field accessors in McapDialog.
  std::string parser_config_override_;
  // Owns the cold (post-import lazy) byte path while fetchers are created.
  // Each fetcher retains the shared cold state, so deferred ObjectStore pulls
  // still work after PJ4 destroys the DataSourceHandle at the end of loadFile().
  mcap::MessageByteStore byte_store_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(McapSource, kMcapManifest)

PJ_DIALOG_PLUGIN(McapDialog)
