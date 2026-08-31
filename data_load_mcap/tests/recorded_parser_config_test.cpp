// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// A PlotJuggler session recording stores, per channel, the parser config the
// live session used (`pj.parser_config` channel metadata). Replaying such a
// file must start from that config so the parse is identical, while the
// dialog's explicit choices and the loader's own keys still win.
//
// Drives the real McapSource against a synthetic recording through a fake
// runtime host that records every ensureParserBinding request — the host stub
// shape mirrors plotjuggler_sdk's file_source_integration_test.cpp, the only
// existing harness in the tree that drives a FileSourceBase end to end.

// The vendored mcap library compiles its bodies into whichever TU defines this;
// it must be defined before the first mcap header, whatever order clang-format
// settles on below. mcap_source.cpp defines it too — a benign redefinition.
#define MCAP_IMPLEMENTATION

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <mcap/writer.hpp>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <string>
#include <vector>

// McapSource lives in an anonymous namespace inside the plugin's .cpp, so this
// test compiles its own copy of that translation unit to drive the real loader
// (the same trick the lerobot dialog test uses for its header-defined dialog).
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

// One schemaful JSON channel, whose metadata carries the parser config of the
// session that recorded it — the shape PlotJuggler's session recorder writes.
std::string writeRecordingWithParserConfig(const std::string& recorded_parser_config) {
  const auto path = std::filesystem::temp_directory_path() / "pj_mcap_recorded_parser_config_test.mcap";

  mcap::McapWriter writer;
  mcap::McapWriterOptions options("test");
  if (!writer.open(path.string(), options).ok()) {
    return {};
  }

  mcap::Schema schema("Point", "json", "{}");
  writer.addSchema(schema);

  mcap::KeyValueMap metadata;
  if (!recorded_parser_config.empty()) {
    metadata["pj.parser_config"] = recorded_parser_config;
  }
  mcap::Channel channel("/pt", "json", schema.id, metadata);
  writer.addChannel(channel);

  const std::string payload = R"({"x":1})";
  mcap::Message msg;
  msg.channelId = channel.id;
  msg.sequence = 1;
  msg.logTime = 1000;
  msg.publishTime = 1000;
  msg.data = reinterpret_cast<const std::byte*>(payload.data());
  msg.dataSize = payload.size();
  const bool written = writer.write(msg).ok();
  writer.close();
  return written ? path.string() : std::string{};
}

class RecordedParserConfigTest : public ::testing::Test {
 protected:
  /// Import @p path through a real McapSource wired to the recording host.
  void importFile(const std::string& path) {
    registry_.registerService<PJ::sdk::SourceWriteHostService>(makeWriteHost(&host_));
    registry_.registerService<PJ::sdk::DataSourceRuntimeHostService>(makeRuntimeHost(&host_));

    ASSERT_TRUE(source_.bind(PJ::sdk::ServiceRegistry(registry_.view())).has_value());
    // The dialog analyzes the file and selects every channel it finds, which
    // is what a user accepting the picker unchanged would produce.
    ASSERT_TRUE(source_.loadConfig(nlohmann::json{{"filepath", path}}.dump()).has_value());
    ASSERT_TRUE(source_.start().has_value());
  }

  HostState host_;
  PJ::ServiceRegistryBuilder registry_;
  McapSource source_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(RecordedParserConfigTest, RecordedConfigSeedsTheBindingAndLoaderKeysStillWin) {
  const std::string path = writeRecordingWithParserConfig(R"({"label_keyed_arrays":true})");
  ASSERT_FALSE(path.empty());

  importFile(path);
  ASSERT_EQ(host_.bindings.size(), 1u);
  EXPECT_EQ(host_.bindings[0].topic_name, "/pt");

  const auto config = nlohmann::json::parse(host_.bindings[0].parser_config_json, nullptr, false);
  ASSERT_TRUE(config.is_object()) << host_.bindings[0].parser_config_json;

  // The recorded config is the base...
  EXPECT_EQ(config.value("label_keyed_arrays", false), true) << host_.bindings[0].parser_config_json;
  // ...and the loader's own per-channel keys survive on top of it.
  EXPECT_EQ(config.value("topic_name", std::string{}), "/pt");
  EXPECT_EQ(config.value("schema_encoding", std::string{}), "json");

  std::filesystem::remove(path);
}

TEST_F(RecordedParserConfigTest, ChannelWithoutRecordedConfigIsUnaffected) {
  const std::string path = writeRecordingWithParserConfig("");
  ASSERT_FALSE(path.empty());

  importFile(path);
  ASSERT_EQ(host_.bindings.size(), 1u);

  const auto config = nlohmann::json::parse(host_.bindings[0].parser_config_json, nullptr, false);
  ASSERT_TRUE(config.is_object()) << host_.bindings[0].parser_config_json;
  EXPECT_FALSE(config.contains("label_keyed_arrays"));
  EXPECT_EQ(config.value("topic_name", std::string{}), "/pt");

  std::filesystem::remove(path);
}

// Metadata is arbitrary user data: a non-object or unparsable value must be
// ignored rather than replacing the config the loader built.
TEST_F(RecordedParserConfigTest, MalformedRecordedConfigIsIgnored) {
  const std::string path = writeRecordingWithParserConfig("not json at all");
  ASSERT_FALSE(path.empty());

  importFile(path);
  ASSERT_EQ(host_.bindings.size(), 1u);

  const auto config = nlohmann::json::parse(host_.bindings[0].parser_config_json, nullptr, false);
  ASSERT_TRUE(config.is_object()) << host_.bindings[0].parser_config_json;
  EXPECT_EQ(config.value("topic_name", std::string{}), "/pt");

  std::filesystem::remove(path);
}

}  // namespace
