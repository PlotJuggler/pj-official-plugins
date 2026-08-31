// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// A PlotJuggler session recording stores, per channel, the parser config the
// live session used (`pj.parser_config` channel metadata). Replaying such a
// file must parse exactly like that session, so the layering is:
//
//   MCAP dialog controls
//     -> recorded pj.parser_config
//       -> explicit "_parser_config" override
//         -> keys derived from the channel being read now
//            (topic_name, serialization, schema_encoding)
//
// Drives the real McapSource against synthetic recordings through a fake
// runtime host that records every ensureParserBinding request — the host stub
// shape mirrors plotjuggler_sdk's file_source_integration_test.cpp, the only
// existing harness in the tree that drives a FileSourceBase end to end.

// The vendored mcap library compiles its bodies into whichever TU defines this;
// it must be defined before the first mcap header, whatever order clang-format
// settles on below. mcap_source.cpp defines it too — a benign redefinition.
#define MCAP_IMPLEMENTATION

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mcap/writer.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// McapSource lives in an anonymous namespace inside the plugin's .cpp, so this
// test compiles its own copy of that translation unit to drive the real loader.
// Including a plugin source (or a header-defined class) into a test TU is the
// established pattern here — see data_load_lerobot's dialog tests.
#include "mcap_source.cpp"  // NOLINT(build/include)

namespace {

// ---------------------------------------------------------------------------
// Fake host
// ---------------------------------------------------------------------------

struct RecordedBinding {
  std::string topic_name;
  std::string parser_encoding;
  std::string type_name;
  std::string parser_config_json;
};

struct HostState {
  std::vector<RecordedBinding> bindings;
  uint64_t pushed_messages = 0;
  std::vector<std::string> messages;
};

// --- source write host (unused by this loader path, but bind() requires it) ---

bool whEnsureTopic(void*, PJ_string_view_t, PJ_topic_handle_t* out, PJ_error_t*) noexcept {
  *out = PJ_topic_handle_t{1};
  return true;
}
bool whEnsureField(
    void*, PJ_topic_handle_t topic, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t* out,
    PJ_error_t*) noexcept {
  *out = PJ_field_handle_t{topic, 1};
  return true;
}
bool whAppendRecord(void*, PJ_topic_handle_t, int64_t, const PJ_named_field_value_t*, uint64_t, PJ_error_t*) noexcept {
  return true;
}
bool whAppendBoundRecord(
    void*, PJ_topic_handle_t, int64_t, const PJ_bound_field_value_t*, uint64_t, PJ_error_t*) noexcept {
  return true;
}
bool whAppendArrowStream(void*, PJ_topic_handle_t, struct ArrowArrayStream*, PJ_string_view_t, PJ_error_t*) noexcept {
  return true;
}

// --- data source runtime host ---

std::string viewToString(PJ_string_view_t view) {
  return view.data == nullptr ? std::string{} : std::string(view.data, view.size);
}

void rhReportMessage(void* ctx, PJ_data_source_message_level_t, PJ_string_view_t message) noexcept {
  static_cast<HostState*>(ctx)->messages.push_back(viewToString(message));
}
bool rhProgressStart(void*, PJ_string_view_t, uint64_t, bool, PJ_error_t*) noexcept {
  return true;
}
bool rhProgressUpdate(void*, uint64_t) noexcept {
  return true;
}
void rhProgressFinish(void*) noexcept {}
bool rhIsStopRequested(void*) noexcept {
  return false;
}
void rhNotifyState(void*, PJ_data_source_state_t) noexcept {}
void rhRequestStop(void*, PJ_data_source_state_t, PJ_string_view_t) noexcept {}

bool rhEnsureParserBinding(
    void* ctx, const PJ_parser_binding_request_t* request, PJ_parser_binding_handle_t* out_handle,
    PJ_error_t*) noexcept {
  auto* state = static_cast<HostState*>(ctx);
  state->bindings.push_back(
      RecordedBinding{
          .topic_name = viewToString(request->topic_name),
          .parser_encoding = viewToString(request->parser_encoding),
          .type_name = viewToString(request->type_name),
          .parser_config_json = viewToString(request->parser_config_json),
      });
  *out_handle = PJ_parser_binding_handle_t{static_cast<uint32_t>(state->bindings.size())};
  return true;
}

int rhShowMessageBox(void*, PJ_message_box_type_t, PJ_string_view_t, PJ_string_view_t, int) noexcept {
  return PJ_MSG_BTN_OK;
}
const char* rhListAvailableEncodings(void*) noexcept {
  return nullptr;
}

// Materialize the bytes the way an eager host does, then release the fetcher —
// otherwise every deferred closure the loader hands over leaks.
bool rhPushMessage(
    void* ctx, PJ_parser_binding_handle_t, int64_t, PJ_message_data_fetcher_t fetcher, PJ_error_t*) noexcept {
  auto* state = static_cast<HostState*>(ctx);
  PJ_payload_t payload{};
  if (fetcher.fetchMessageData != nullptr && fetcher.fetchMessageData(fetcher.ctx, &payload, nullptr)) {
    ++state->pushed_messages;
    if (payload.anchor.release != nullptr) {
      payload.anchor.release(payload.anchor.ctx);
    }
  }
  if (fetcher.release != nullptr) {
    fetcher.release(fetcher.ctx);
  }
  return true;
}

bool rhNotifyAvailableTopics(void*, const PJ_available_topic_t*, uint64_t, PJ_error_t*) noexcept {
  return true;
}

PJ_source_write_host_t makeWriteHost(HostState* state) {
  static const PJ_source_write_host_vtable_t vtable = {
      .abi_version = PJ_PLUGIN_DATA_API_VERSION,
      .struct_size = sizeof(PJ_source_write_host_vtable_t),
      .ensure_topic = whEnsureTopic,
      .ensure_field = whEnsureField,
      .append_record = whAppendRecord,
      .append_bound_record = whAppendBoundRecord,
      .append_arrow_stream = whAppendArrowStream,
  };
  return PJ_source_write_host_t{.ctx = state, .vtable = &vtable};
}

PJ_data_source_runtime_host_t makeRuntimeHost(HostState* state) {
  static const PJ_data_source_runtime_host_vtable_t vtable = {
      .protocol_version = 1,
      .struct_size = sizeof(PJ_data_source_runtime_host_vtable_t),
      .report_message = rhReportMessage,
      .progress_start = rhProgressStart,
      .progress_update = rhProgressUpdate,
      .progress_finish = rhProgressFinish,
      .is_stop_requested = rhIsStopRequested,
      .notify_state = rhNotifyState,
      .request_stop = rhRequestStop,
      .ensure_parser_binding = rhEnsureParserBinding,
      .show_message_box = rhShowMessageBox,
      .list_available_encodings = rhListAvailableEncodings,
      .push_message = rhPushMessage,
      .notify_available_topics = rhNotifyAvailableTopics,
  };
  return PJ_data_source_runtime_host_t{.ctx = state, .vtable = &vtable};
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

/// Unique directory under the system temp dir, removed with everything in it.
/// Unique rather than a fixed name so concurrent ctest jobs (and a crashed
/// previous run's leftovers) cannot feed one test another's recording.
class TempDir {
 public:
  TempDir() {
    static std::atomic<uint64_t> counter{0};
    static const uint64_t salt = std::random_device{}();
    path_ =
        std::filesystem::temp_directory_path() / ("mcap_rec_" + std::to_string(salt) + "_" + std::to_string(counter++));
    std::filesystem::create_directories(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] std::string file(std::string_view name) const {
    return (path_ / name).string();
  }

 private:
  std::filesystem::path path_;
};

/// One channel of a synthetic recording. `recorded_parser_config` is written
/// verbatim as the channel's `pj.parser_config` metadata; nullopt writes no
/// metadata at all, the shape every non-PlotJuggler MCAP has.
struct ChannelSpec {
  std::string topic;
  std::optional<std::string> recorded_parser_config;
};

class RecordedParserConfigTest : public ::testing::Test {
 protected:
  /// Write a recording whose channels all share one JSON schema and carry one
  /// message each. Returns the file path.
  std::string writeRecording(const std::vector<ChannelSpec>& channels) {
    const std::string path = dir_.file("recording.mcap");

    mcap::McapWriter writer;
    mcap::McapWriterOptions options("test");
    EXPECT_TRUE(writer.open(path, options).ok());

    mcap::Schema schema("Point", "json", "{}");
    writer.addSchema(schema);

    const std::string payload = R"({"x":1})";
    for (const auto& spec : channels) {
      mcap::KeyValueMap metadata;
      if (spec.recorded_parser_config.has_value()) {
        metadata["pj.parser_config"] = *spec.recorded_parser_config;
      }
      mcap::Channel channel(spec.topic, "json", schema.id, metadata);
      writer.addChannel(channel);

      mcap::Message msg;
      msg.channelId = channel.id;
      msg.sequence = 1;
      msg.logTime = 1000;
      msg.publishTime = 1000;
      msg.data = reinterpret_cast<const std::byte*>(payload.data());
      msg.dataSize = payload.size();
      EXPECT_TRUE(writer.write(msg).ok());
    }
    writer.close();
    return path;
  }

  /// Import @p path through a real McapSource wired to the recording host.
  /// @p parser_config_override, when non-empty, is embedded exactly the way
  /// FileLoader does it: the JSON *text* of the parser config, stored as a
  /// string under "_parser_config" in the source's own config.
  void importFile(const std::string& path, const std::string& parser_config_override = {}) {
    registry_.registerService<PJ::sdk::SourceWriteHostService>(makeWriteHost(&host_));
    registry_.registerService<PJ::sdk::DataSourceRuntimeHostService>(makeRuntimeHost(&host_));

    nlohmann::json config{{"filepath", path}};
    if (!parser_config_override.empty()) {
      config["_parser_config"] = parser_config_override;
    }

    ASSERT_TRUE(source_.bind(PJ::sdk::ServiceRegistry(registry_.view())).has_value());
    // The dialog analyzes the file and selects every channel it finds, which is
    // what a user accepting the picker unchanged would produce.
    ASSERT_TRUE(source_.loadConfig(config.dump()).has_value());
    ASSERT_TRUE(source_.start().has_value());
  }

  /// The parser config the loader requested for @p topic. Channels are bound in
  /// hash order, so every lookup goes through the topic name.
  nlohmann::json configFor(std::string_view topic) const {
    for (const auto& binding : host_.bindings) {
      if (binding.topic_name == topic) {
        auto config = nlohmann::json::parse(binding.parser_config_json, nullptr, false);
        EXPECT_TRUE(config.is_object()) << binding.parser_config_json;
        return config;
      }
    }
    ADD_FAILURE() << "no parser binding was requested for " << topic;
    return nlohmann::json::object();
  }

  TempDir dir_;
  HostState host_;
  PJ::ServiceRegistryBuilder registry_;
  McapSource source_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(RecordedParserConfigTest, RecordedConfigWinsOverDialogDefaultsButNotOverLoaderKeys) {
  // use_embedded_timestamp and max_array_size are keys the dialog writes on
  // every import from its own (here untouched, default) state: false and 500.
  // The trailing three are deliberately stale copies of the loader-derived
  // keys, so the assertions below cannot pass vacuously.
  const std::string path = writeRecording(
      {{"/pt", R"({"label_keyed_arrays":true,"use_embedded_timestamp":true,)"
               R"("max_array_size":5000,"topic_name":"/recorded_as_something_else",)"
               R"("schema_encoding":"protobuf","serialization":"ros1"})"}});

  importFile(path);
  ASSERT_EQ(host_.bindings.size(), 1u);

  const auto config = configFor("/pt");
  const std::string dumped = config.dump();
  // A key only the recording carries.
  EXPECT_EQ(config.value("label_keyed_arrays", false), true) << dumped;
  // Keys the dialog also writes, from its defaults: the recording wins.
  EXPECT_EQ(config.value("use_embedded_timestamp", false), true) << dumped;
  EXPECT_EQ(config.value("max_array_size", 0), 5000) << dumped;
  // Keys derived from the channel being read now: the loader still wins.
  EXPECT_EQ(config.value("topic_name", std::string{}), "/pt");
  EXPECT_EQ(config.value("schema_encoding", std::string{}), "json");
  EXPECT_EQ(config.value("serialization", std::string{}), "cdr");
}

// The config is built per channel from a shared base. A base mutated in place
// would leak one channel's recorded keys into the next.
TEST_F(RecordedParserConfigTest, EachChannelGetsItsOwnRecordedConfig) {
  const std::string path = writeRecording({
      {"/a", R"({"label_keyed_arrays":true,"max_array_size":11,"only_on_a":"a"})"},
      {"/b", R"({"label_keyed_arrays":false,"max_array_size":22,"only_on_b":"b"})"},
  });

  importFile(path);
  ASSERT_EQ(host_.bindings.size(), 2u);

  const auto config_a = configFor("/a");
  EXPECT_EQ(config_a.value("label_keyed_arrays", false), true);
  EXPECT_EQ(config_a.value("max_array_size", 0), 11);
  EXPECT_EQ(config_a.value("only_on_a", std::string{}), "a");
  EXPECT_FALSE(config_a.contains("only_on_b")) << config_a.dump();
  EXPECT_EQ(config_a.value("topic_name", std::string{}), "/a");

  const auto config_b = configFor("/b");
  EXPECT_EQ(config_b.value("label_keyed_arrays", true), false);
  EXPECT_EQ(config_b.value("max_array_size", 0), 22);
  EXPECT_EQ(config_b.value("only_on_b", std::string{}), "b");
  EXPECT_FALSE(config_b.contains("only_on_a")) << config_b.dump();
  EXPECT_EQ(config_b.value("topic_name", std::string{}), "/b");
}

// The common mixed file: a recorded topic next to one the recorder never saw.
TEST_F(RecordedParserConfigTest, ChannelWithoutRecordedConfigKeepsTheDialogDefaults) {
  const std::string path = writeRecording({
      {"/recorded", R"({"label_keyed_arrays":true,"max_array_size":5000})"},
      {"/plain", std::nullopt},
  });

  importFile(path);
  ASSERT_EQ(host_.bindings.size(), 2u);

  const auto plain = configFor("/plain");
  EXPECT_FALSE(plain.contains("label_keyed_arrays")) << plain.dump();
  EXPECT_EQ(plain.value("max_array_size", 0), 500) << plain.dump();
  EXPECT_EQ(plain.value("use_embedded_timestamp", true), false);
  EXPECT_EQ(plain.value("topic_name", std::string{}), "/plain");

  // ...and the sibling still gets its recorded values.
  EXPECT_EQ(configFor("/recorded").value("max_array_size", 0), 5000);
}

// Channel metadata is arbitrary user data. Anything that is not a JSON object —
// valid JSON of another type, or not JSON at all — must be ignored rather than
// replacing or corrupting the config the loader built.
TEST_F(RecordedParserConfigTest, NonObjectAndMalformedRecordedValuesAreIgnored) {
  const std::string path = writeRecording({
      {"/array", "[]"},
      {"/null", "null"},
      {"/text", R"("text")"},
      {"/garbage", "not json at all"},
      {"/empty", ""},
  });

  importFile(path);
  ASSERT_EQ(host_.bindings.size(), 5u);

  for (const std::string_view topic : {"/array", "/null", "/text", "/garbage", "/empty"}) {
    const auto config = configFor(topic);
    EXPECT_EQ(config.value("max_array_size", 0), 500) << topic << ": " << config.dump();
    EXPECT_EQ(config.value("use_embedded_timestamp", true), false) << topic;
    EXPECT_EQ(config.value("topic_name", std::string{}), topic) << config.dump();
  }
}

// An explicit override (a preset, a saved layout, or a parser sub-dialog) is a
// deliberate choice about THIS import and outranks the recording.
TEST_F(RecordedParserConfigTest, ExplicitOverrideOutranksTheRecordedConfig) {
  const std::string path = writeRecording(
      {{"/pt", R"({"label_keyed_arrays":true,"use_embedded_timestamp":true,)"
               R"("max_array_size":5000,"schema_encoding":"protobuf"})"}});

  // Embedded exactly as FileLoader does it: the parser config's JSON text,
  // stored as a string value under "_parser_config".
  importFile(path, R"({"max_array_size":77,"use_embedded_timestamp":false,"only_in_override":true})");
  ASSERT_EQ(host_.bindings.size(), 1u);

  const auto config = configFor("/pt");
  const std::string dumped = config.dump();
  // Keys both the recording and the override set: the override wins.
  EXPECT_EQ(config.value("max_array_size", 0), 77) << dumped;
  EXPECT_EQ(config.value("use_embedded_timestamp", true), false) << dumped;
  // Keys only one of them sets: both survive.
  EXPECT_EQ(config.value("only_in_override", false), true) << dumped;
  EXPECT_EQ(config.value("label_keyed_arrays", false), true) << dumped;
  // The loader-derived keys still outrank everything.
  EXPECT_EQ(config.value("topic_name", std::string{}), "/pt");
  EXPECT_EQ(config.value("schema_encoding", std::string{}), "json") << dumped;
}

}  // namespace
