#include <pj_base/builtin/builtin_object.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_plugins/sdk/parser_array_policy.hpp>

#define MCAP_IMPLEMENTATION
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mcap_dataset_metadata.hpp"
#include "mcap_dialog.hpp"
#include "mcap_helpers.hpp"
#include "mcap_manifest.hpp"

// Vendored parallel-reader fork (contrib/mcap/). Pulled in after reader.hpp
// with MCAP_IMPLEMENTATION already defined, so parallel_reader.hpp's trailing
// #include "parallel_reader.inl" compiles the parallel-reader bodies here (the
// same .hpp/.inl split the rest of the library uses). message_byte_store.hpp
// adds the optional deferred-byte (hot/cold lazy) layer on top.
#include <mcap/message_byte_store.hpp>       // NOLINT(build/include_order)
#include <mcap/parallel_reader.hpp>          // NOLINT(build/include_order)
#include <mcap/unchunked_payload_store.hpp>  // NOLINT(build/include_order)

namespace {

struct TransparentStringHash {
  using is_transparent = void;

  std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }

  std::size_t operator()(const std::string& value) const noexcept {
    return (*this)(std::string_view(value));
  }
};

using SelectedTopicSet = std::unordered_set<std::string, TransparentStringHash, std::equal_to<>>;

// Hardware-derived worker count. Floor at 2 so we still get parallelism on
// virtualized hosts that report 1 core; cap at 8 to avoid oversubscription on
// many-core machines where the byte-budget and consumer drain rate become the
// real limits anyway.
inline unsigned parallelImportThreadCount() {
#ifdef PJ_TARGET_WASM
  // Browser pthreads are comparatively expensive and WASM memory growth
  // expands in coarse steps. Two workers keep decompression parallel without
  // multiplying the resident chunk frontier.
  return 2;
#else
  const unsigned hw = std::thread::hardware_concurrency();
  return std::min(8u, std::max(2u, hw));
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// McapSource plugin
// ─────────────────────────────────────────────────────────────────────────────

class McapSource : public PJ::FileSourceBase {
#ifdef PJ_TARGET_WASM
  static constexpr size_t kChunkCacheCapacityBytes = 32ULL * 1024 * 1024;
  static constexpr uint64_t kParallelImportBudgetBytes = 128ULL * 1024 * 1024;
  static constexpr uint64_t kParallelLookaheadBytes = 64ULL * 1024 * 1024;
  static constexpr uint64_t kMaxChunkUncompressedBytes = 128ULL * 1024 * 1024;
#else
  // Cold-path chunk cache budget, handed to the MessageByteStore. 128 MiB
  // comfortably holds tens of typical 4 MiB chunks so a scrubbing consumer
  // rarely re-decompresses, while staying well below host memory pressure.
  // Only post-import lazy pulls populate it — eager import-time fetches come
  // straight from the parallel reader's pinned chunk (zero-copy, no cache).
  static constexpr size_t kChunkCacheCapacityBytes = 128ULL * 1024 * 1024;
  // Parallel-import in-flight decompression budget.
  static constexpr uint64_t kParallelImportBudgetBytes = 256ULL * 1024 * 1024;
#endif
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

    // One file for a plain .mcap; every split, in recording order, for a
    // rosbag2 metadata.yaml. The splits import as ONE dataset: topics bind
    // once and the files are read back to back.
    const auto recording = PJ::McapHelpers::resolveRecordingFiles(dialog_.filepath());
    if (!recording.error.empty()) {
      runtimeHost().showError("MCAP import failed", recording.error);
      return PJ::unexpected(recording.error);
    }
    const bool multi_file = recording.paths.size() > 1;

    // open(path) gives each reader an internally-owned ConcurrentFileReader
    // (positioned pread/ReadFile, concurrent-safe). Declared before `messages`
    // below so they outlive the message views. open() runs an
    // AllowFallbackScan summary, so channels, schemas, statistics and chunk
    // indexes are all available now — no separate reader or summary parse is
    // needed for binding setup or the cold path.
    std::vector<std::unique_ptr<mcap::ParallelReader>> readers;
    for (const std::string& path : recording.paths) {
      auto reader = std::make_unique<mcap::ParallelReader>();
      if (auto st = reader->open(path); !st.ok()) {
        const std::string msg =
            std::string("cannot open MCAP file") + (multi_file ? " " + path : std::string{}) + ": " + st.message;
        runtimeHost().showError("MCAP import failed", msg);
        return PJ::unexpected(msg);
      }
      readers.push_back(std::move(reader));
    }
    // channels() returns the table BY VALUE: take one copy per file up front and
    // share it between the progress total and the binding loop below.
    std::vector<std::unordered_map<mcap::ChannelId, mcap::ChannelPtr>> channels_per_file;
    for (const auto& reader : readers) {
      channels_per_file.push_back(reader->channels());
    }

    // Dataset-metadata document: file facts + every file-level Metadata record
    // (PJ's pj.capture / pj.recording parsed in). Extracted up front from the
    // summary the open already paid for — never lazily. Extraction problems
    // are warnings; they must not affect the import.
    {
      std::vector<std::string> metadata_diagnostics;
      std::vector<mcap::McapReader*> summary_readers;
      for (const auto& reader : readers) {
        summary_readers.push_back(&reader->reader());
      }
      const nlohmann::json metadata_document =
          PJ::McapMetadata::extractDatasetMetadata(summary_readers, &metadata_diagnostics);
      for (const std::string& diagnostic : metadata_diagnostics) {
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, diagnostic);
      }
      PJ::McapMetadata::publishDatasetMetadata(runtimeHost(), metadata_document);
    }

    const auto& selected = dialog_.selectedTopics();

    // Progress is measured against the messages this import will actually push,
    // i.e. only the channels the user selected. The file-wide messageCount
    // would leave the bar stranded well short of its maximum whenever the
    // selection is a subset (the common case).
    uint64_t total_messages = 0;
    for (size_t file_index = 0; file_index < readers.size(); ++file_index) {
      if (const auto stats = readers[file_index]->statistics()) {
        std::vector<mcap::ChannelId> selected_channel_ids;
        for (const auto& [channel_id, channel_ptr] : channels_per_file[file_index]) {
          if (selected.find(channel_ptr->topic) != selected.end()) {
            selected_channel_ids.push_back(channel_id);
          }
        }
        total_messages += PJ::McapHelpers::progressTotal(*stats, selected_channel_ids);
      }
    }
    (void)runtimeHost().progressStart("Importing MCAP", total_messages, true);

    // Parser config starts with the embedded parser dialog (pj_parser_slot) when
    // present, then MCAP-level controls apply consistently to every selected
    // channel.
    nlohmann::json parser_config = nlohmann::json::object();
    if (!parser_config_override_.empty()) {
      auto parsed_config = nlohmann::json::parse(parser_config_override_, nullptr, false);
      if (parsed_config.is_object()) {
        parser_config = std::move(parsed_config);
      }
    }
    PJ::sdk::arrayLimitToJson(parser_config, static_cast<uint32_t>(dialog_.maxArraySize()), dialog_.clampLargeArrays());
    parser_config["use_embedded_timestamp"] = dialog_.useHeaderTimestamp();

    // --- Ensure parser bindings for selected channels ---
    // Channel ids are per file, topics are not: each split of a bag gets its
    // own channel-id -> binding map, but a topic binds ONCE (first file that
    // carries it) so every file feeds the same series.
    using BindingMap = std::unordered_map<mcap::ChannelId, PJ::ParserBindingHandle>;
    std::vector<BindingMap> bindings_per_file(readers.size());
    std::unordered_map<std::string, std::optional<PJ::ParserBindingHandle>> binding_by_topic;
    std::vector<std::string> binding_errors;
    bool any_binding = false;

    for (size_t file_index = 0; file_index < readers.size(); ++file_index) {
      const auto& channels = channels_per_file[file_index];
      const auto schemas = readers[file_index]->schemas();
      BindingMap& bindings = bindings_per_file[file_index];
      for (const auto& [channel_id, channel_ptr] : channels) {
        // Filter by dialog selection.
        if (selected.find(channel_ptr->topic) == selected.end()) {
          continue;
        }
        if (auto known = binding_by_topic.find(channel_ptr->topic); known != binding_by_topic.end()) {
          if (known->second) {
            bindings.emplace(channel_id, *known->second);
          }
          continue;
        }

        // schema_id 0 is the MCAP spec's "no schema" sentinel, not a broken
        // reference: the payload is self-describing (schemaless JSON is the
        // common case) and the parser is selected from the message encoding
        // alone. Schema ids are 1-based, so a NON-zero id that resolves to
        // nothing is a dangling reference in a damaged file and stays skipped.
        // Mirrors the channel filter in McapDialog::analyzeFile.
        const bool schemaless = PJ::McapHelpers::isSchemaless(channel_ptr->schemaId);
        auto schema_it = schemas.find(channel_ptr->schemaId);
        if (!schemaless && schema_it == schemas.end()) {
          continue;
        }
        const mcap::Schema* schema = schemaless ? nullptr : schema_it->second.get();

        PJ::Span<const uint8_t> schema_bytes{};
        if (schema != nullptr) {
          schema_bytes =
              PJ::Span<const uint8_t>{reinterpret_cast<const uint8_t*>(schema->data.data()), schema->data.size()};
        }

        std::string parser_encoding = channel_ptr->messageEncoding;
        if (schema != nullptr && (parser_encoding.empty() || (parser_encoding == "cdr" && !schema->encoding.empty()))) {
          parser_encoding = schema->encoding;
        }
        if (parser_encoding.empty()) {
          // Neither a schema nor a message encoding: nothing identifies the
          // payload format. Report it rather than dropping the topic silently.
          binding_errors.push_back(channel_ptr->topic + ": channel declares neither a schema nor a message encoding");
          binding_by_topic.emplace(channel_ptr->topic, std::nullopt);
          continue;
        }

        auto channel_parser_config = parser_config;
        const bool use_ros1_serialization = channel_ptr->messageEncoding == "ros1" || parser_encoding == "ros1" ||
                                            parser_encoding == "ros1msg" ||
                                            (schema != nullptr && schema->encoding == "ros1msg");
        channel_parser_config["serialization"] = use_ros1_serialization ? "ros1" : "cdr";
        channel_parser_config["schema_encoding"] = parser_encoding;
        // Topic-conditional parser classifications (notably std_msgs/String on
        // */robot_description) need the same topic identity already carried by
        // ParserBindingRequest. Keep it in each parser instance's config too so
        // classifySchema() and the retained object parser agree.
        channel_parser_config["topic_name"] = channel_ptr->topic;
        const std::string parser_config_str = channel_parser_config.dump();

        PJ::ParserBindingRequest request{
            .topic_name = channel_ptr->topic,
            .parser_encoding = parser_encoding,
            .type_name = schema != nullptr ? schema->name : channel_ptr->topic,
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
          binding_by_topic.emplace(channel_ptr->topic, std::nullopt);
          continue;
        }
        binding_by_topic.emplace(channel_ptr->topic, *handle);
        bindings.emplace(channel_id, *handle);
        any_binding = true;
      }
    }

    if (!any_binding) {
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

    // Cold-path byte stores, ONE PER FILE, each seeded from that file's
    // parallel-reader chunk index (no extra summary parse). Members, so they —
    // and the fetchers that share their cold state — outlive importData():
    // post-import lazy pulls re-decompress on demand through the store of the
    // file the message came from, which keeps its own FileReader open for the
    // rest of the session. That reader is opened lazily on the first cold
    // miss, so fully-eager imports never reopen a file.
    byte_stores_.clear();
    byte_stores_.resize(readers.size());
    for (size_t file_index = 0; file_index < readers.size(); ++file_index) {
      byte_stores_[file_index].init(
          recording.paths[file_index], readers[file_index]->chunkIndexes(),
          {.cacheCapacityBytes = kChunkCacheCapacityBytes}, [this](const mcap::Status& problem) {
            runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, problem.message);
          });
    }

    // --- Iterate messages via the parallel reader ---
    // The fork decompresses chunks on a worker pool ahead of the merge frontier
    // and keeps each chunk pinned only while a message from it is being emitted.
    // For every message we hand the host a MessageByteFetcher:
    //   * invoked eagerly/synchronously during this loop, it returns a zero-copy
    //     view into the still-pinned, worker-decompressed bytes (no re-decompress);
    //   * invoked lazily after import, the pinned chunk is gone, so it falls back
    //     to the byte store's bounded cold re-decompression. True lazy.
    // LinearMessageView (the indexless branch) exposes no status(), so this
    // callback is that branch's only failure channel. Only problems that
    // actually end the scan belong in first_problem — see
    // PJ::McapHelpers::problemIsTerminal — while every problem is still
    // surfaced to the user as a warning.
    std::optional<mcap::Status> first_problem;
    auto on_problem = [&](const mcap::Status& problem) {
      if (!first_problem && PJ::McapHelpers::problemIsTerminal(problem.code)) {
        first_problem = problem;
      }
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning, problem.message);
    };

    uint64_t msg_count = 0;       // messages processed (drives the progress bar)
    uint64_t accepted_count = 0;  // messages the host actually accepted
    uint64_t consecutive_push_failures = 0;
    const bool use_log_time = dialog_.useLogTime();

    // Authoritative recording window (log-time envelope) from the MCAP summary,
    // already read by parallel_reader.open(). Header (publishTime) stamps that
    // fall outside it come from latched / long-running publishers — static TF,
    // robot_description, a map computed hours or days earlier — and would drag
    // the playback range off the actual recording. For those we fall back to the
    // message's own logTime (see the per-message timestamp below). If the summary
    // has no statistics the window stays fully open and behaviour is unchanged.
    // For a split bag the window spans the WHOLE recording, not each file: a
    // header stamp in file 1 that lands in file 2's time range is still valid.
    uint64_t log_window_min = 0;
    uint64_t log_window_max = UINT64_MAX;
    const bool all_have_statistics = std::all_of(
        readers.begin(), readers.end(), [](const auto& reader) { return reader->statistics().has_value(); });
    if (all_have_statistics) {
      // An empty split (recorder stopped right after a rollover) reports
      // start/end time 0; folding it in would open the window down to 0.
      uint64_t lo = UINT64_MAX;
      uint64_t hi = 0;
      for (const auto& reader : readers) {
        const auto& stats = *reader->statistics();
        if (stats.messageCount > 0) {
          lo = std::min(lo, stats.messageStartTime);
          hi = std::max(hi, stats.messageEndTime);
        }
      }
      if (lo <= hi) {
        log_window_min = lo;
        log_window_max = hi;
      }
    }

    mcap::ParallelReadOptions parallel_opts;
    parallel_opts.maxBytesInFlight = kParallelImportBudgetBytes;
    parallel_opts.threadCount = parallelImportThreadCount();
#ifdef PJ_TARGET_WASM
    parallel_opts.lookaheadBytes = kParallelLookaheadBytes;
    parallel_opts.maxChunkUncompressedSize = kMaxChunkUncompressedBytes;
#endif
    const auto& dialog_selected_topics = dialog_.selectedTopics();
    SelectedTopicSet selected_topics(dialog_selected_topics.begin(), dialog_selected_topics.end());
    parallel_opts.read.topicFilter = [selected_topics = std::move(selected_topics)](std::string_view topic) {
      return selected_topics.contains(topic);
    };

    // Track terminal failure so we can return PJ::unexpected after the message
    // view and parallel reader cleanly destruct. A failed push run or a thrown
    // exception must propagate to FileLoader as a load failure, not get silently
    // downgraded to okStatus() with a warning toast.
    std::optional<std::string> import_failure;

    // Final status of the recording: the first failing file's status. The
    // serial branch has no status() and reports through on_problem, the
    // parallel one exposes status() directly. Classified ONCE after the read
    // finishes rather than inline, so the "did this import actually succeed"
    // rule lives in one testable place — PJ::McapHelpers::classifyImportOutcome.
    mcap::Status view_status;

    // Bindings of the file currently being read (channel ids are per file).
    const BindingMap* bindings = nullptr;
    // Set when a push returns false (cancel or push-failure abort): stops the
    // whole recording, not just the current file.
    bool stopped = false;

    auto push_message = [&](const mcap::MessageView& mv, auto fetch_payload) {
      if (mv.channel == nullptr || mv.message.data == nullptr) {
        return true;
      }
      auto binding_it = bindings->find(mv.channel->id);
      if (binding_it == bindings->end()) {
        return true;
      }

      const uint64_t pub_time = mv.message.publishTime;
      const bool header_in_window = pub_time >= log_window_min && pub_time <= log_window_max;
      // Out-of-window publish stamps fall back to this message's OWN logTime —
      // when it was actually recorded, which is inside the window by
      // construction. Collapsing them onto log_window_min instead would stack
      // every latched message on one instant and destroy their relative order.
      const PJ::Timestamp timestamp_ns =
          use_log_time ? static_cast<PJ::Timestamp>(mv.message.logTime)
                       : static_cast<PJ::Timestamp>(header_in_window ? pub_time : mv.message.logTime);

      auto push_status = runtimeHost().pushMessage(binding_it->second, timestamp_ns, std::move(fetch_payload));
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
          return false;
        }
      } else {
        consecutive_push_failures = 0;
        ++accepted_count;
      }

      ++msg_count;
      if (msg_count % kProgressUpdateInterval == 0 &&
          (!runtimeHost().progressUpdate(msg_count) || runtimeHost().isStopRequested())) {
        return false;
      }
      return true;
    };

    // Serial file-order scan. Two callers: the primary path for recordings
    // that have no message indexes at all, and the recovery path when an
    // indexed read fails outright (see shouldRetryInFileOrder below).
    auto read_file_order = [&](mcap::ParallelReader& parallel_reader, const std::string& path) {
      // Indexless serial fallback. A chunk-free recording keeps every
      // Message record uncompressed at a stable file offset, so each fetcher
      // retains only a small locator and re-reads its payload on demand —
      // retention stays bounded no matter how many object-topic messages the
      // host defers (ObjectStore keeps those fetchers for the whole
      // session). Only a chunked recording whose summary lacks message
      // indexes still pays a message-sized eager copy: its in-chunk messages
      // have no stable file offset to re-read from.
      auto payload_store = std::make_shared<mcap::UnchunkedPayloadStore>(path);
      auto messages = parallel_reader.reader().readMessages(on_problem, parallel_opts.read);
      for (const auto& mv : messages) {
        if (mv.channel == nullptr || mv.message.data == nullptr) {
          continue;
        }
        bool keep_going = true;
        if (mv.messageOffset.chunkOffset.has_value()) {
          const auto* data = reinterpret_cast<const uint8_t*>(mv.message.data);
          auto bytes = std::make_shared<const std::vector<uint8_t>>(data, data + mv.message.dataSize);
          keep_going = push_message(mv, [bytes]() { return PJ::sdk::PayloadView{bytes}; });
        } else {
          keep_going = push_message(
              mv, [payload_store, record_offset = mv.messageOffset.offset, size = mv.message.dataSize,
                   channel_id = mv.channel->id, log_time = mv.message.logTime]() {
                mcap::ByteView view = payload_store->fetch(record_offset, size, channel_id, log_time);
                return PJ::sdk::PayloadView{
                    PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(view.data), view.size),
                    view.anchor,
                };
              });
        }
        if (!keep_going) {
          stopped = true;
          break;
        }
      }
    };

    // Final status of the file being read (same two sources as view_status).
    mcap::Status file_status;

    try {
      for (size_t file_index = 0; file_index < readers.size() && !stopped; ++file_index) {
        mcap::ParallelReader& parallel_reader = *readers[file_index];
        const std::string& path = recording.paths[file_index];
        bindings = &bindings_per_file[file_index];
        if (bindings->empty()) {
          continue;  // none of the selected topics in this split
        }
        const uint64_t pushed_before = msg_count;
        const uint64_t accepted_before = accepted_count;
        first_problem.reset();
        file_status = mcap::Status{};

        // Recordings without Message Index records cannot be replayed in
        // LogTimeOrder: the reader rejects that order and otherwise yields a
        // misleading empty import. Their physical file order is the only
        // available deterministic order. Note this is NOT the same as "has no
        // chunk indexes" — see PJ::McapHelpers::lacksMessageIndexes for the
        // two cases that carry chunk indexes with no message indexes behind them.
        const bool lacks_message_indexes = PJ::McapHelpers::lacksMessageIndexes(parallel_reader.chunkIndexes());
        parallel_opts.read.readOrder = lacks_message_indexes ? mcap::ReadMessageOptions::ReadOrder::FileOrder
                                                             : mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;

        if (lacks_message_indexes) {
          read_file_order(parallel_reader, path);
          if (first_problem) {
            file_status = *first_problem;
          }
        } else {
          const mcap::MessageByteStore& byte_store = byte_stores_[file_index];
          auto messages = parallel_reader.readMessages(on_problem, parallel_opts);
          for (auto it = messages.begin(); it != messages.end(); ++it) {
            const auto& mv = *it;
            if (!push_message(mv, [fetcher = byte_store.makeFetcher(it, mv)]() {
                  mcap::ByteView v = fetcher();
                  return PJ::sdk::PayloadView{
                      PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(v.data), v.size),
                      v.anchor,
                  };
                })) {
              stopped = true;
              break;
            }
          }
          file_status = messages.status();

          // The summary advertised message indexes, so we came down the indexed
          // path — but the index records themselves can be truncated or corrupt,
          // and then the reader dies having produced nothing. A file-order scan
          // never consults them, so it can still recover the whole file. Only
          // reachable when the first pass delivered nothing from THIS file, so
          // there is no risk of ingesting anything twice.
          if (PJ::McapHelpers::shouldRetryInFileOrder(
                  file_status.ok(), msg_count - pushed_before, accepted_count - accepted_before,
                  runtimeHost().isStopRequested())) {
            runtimeHost().reportMessage(
                PJ::DataSourceMessageLevel::kWarning,
                "MCAP indexed read failed (" + file_status.message + "); retrying as a serial file-order scan");
            parallel_opts.read.readOrder = mcap::ReadMessageOptions::ReadOrder::FileOrder;
            first_problem.reset();
            file_status = mcap::Status{};
            read_file_order(parallel_reader, path);
            if (first_problem) {
              file_status = *first_problem;
            }
          }
        }

        // A damaged split (typically the last one, cut by a killed recorder)
        // does not stop the rest of the bag; the first failure is what the
        // outcome below reports.
        if (!file_status.ok() && view_status.ok()) {
          view_status = file_status;
          if (multi_file) {
            view_status.message = path + ": " + view_status.message;
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

    // A push-failure abort or a thrown exception already recorded a specific
    // reason; it wins over anything the outcome rule would say, and must not
    // be overwritten by the more generic view-status message.
    if (import_failure) {
      return PJ::unexpected(*import_failure);
    }

    switch (PJ::McapHelpers::classifyImportOutcome(
        view_status.ok(), msg_count, accepted_count, runtimeHost().isStopRequested())) {
      case PJ::McapHelpers::ImportOutcome::kSuccess:
        break;

      case PJ::McapHelpers::ImportOutcome::kPartial: {
        // Error level rather than warning: the dataset that just landed is
        // INCOMPLETE and every downstream analysis silently inherits that. The
        // load still succeeds — keeping recovered data is what the pre-SDK
        // plugin did — so this message is the only signal the user ever gets.
        std::string partial =
            "MCAP file partially recovered: " + view_status.message + " (loaded " + std::to_string(accepted_count);
        if (total_messages > 0) {
          partial += " of " + std::to_string(total_messages);
        }
        partial += " messages)";
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kError, partial);
        break;
      }

      case PJ::McapHelpers::ImportOutcome::kFailed: {
        const std::string msg = view_status.ok() ? ("MCAP import loaded no messages: all " + std::to_string(msg_count) +
                                                    " message(s) on the selected topics were rejected by the host.")
                                                 : ("MCAP import failed: " + view_status.message);
        runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kError, msg);
        return PJ::unexpected(msg);
      }
    }
    return PJ::okStatus();
  }

 private:
  McapDialog dialog_;
  // Parser config from the embedded parser dialog (pj_parser_slot). Set by
  // loadConfig() when FileLoader embeds it under "_parser_config". When
  // non-empty, takes precedence over per-field accessors in McapDialog.
  std::string parser_config_override_;
  // Own the cold (post-import lazy) byte path while fetchers are created, one
  // store per recording file (a split rosbag2 bag has several). Each fetcher
  // retains its file's shared cold state, so deferred ObjectStore pulls still
  // work after PJ4 destroys the DataSourceHandle at the end of loadFile().
  std::vector<mcap::MessageByteStore> byte_stores_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(McapSource, kMcapManifest)

PJ_DIALOG_PLUGIN(McapDialog, kMcapManifest)
