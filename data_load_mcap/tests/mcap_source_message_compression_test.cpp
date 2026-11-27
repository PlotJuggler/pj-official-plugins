// Drives the BUILT mcap_source_plugin through ROS 2 bags recorded with `compression_mode: message`: every payload is a
// compressed frame, and only the file's own `rosbag2` Metadata record says so.
#define MCAP_IMPLEMENTATION
#include <gtest/gtest.h>
#include <zstd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mcap/writer.hpp>
#include <optional>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/host/data_source_library.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <pj_plugins/testing/delegated_ingest_fixture.hpp>
#include <pj_utf8_path/utf8_path.hpp>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

const std::vector<uint8_t> kCdrInt32{0x00, 0x01, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00};

std::string bareRosbag2Body(const std::string& format, const std::string& mode) {
  return "version: 9\n"
         "storage_identifier: mcap\n"
         "duration:\n"
         "  nanoseconds: 0\n"
         "compression_format: " +
         format +
         "\n"
         "compression_mode: " +
         mode +
         "\n"
         "relative_file_paths:\n"
         "  - rec_0.mcap\n";
}

std::vector<uint8_t> zstdFrame(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out(ZSTD_compressBound(in.size()));
  const size_t written = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3);
  EXPECT_EQ(ZSTD_isError(written), 0u);
  out.resize(written);
  return out;
}

struct FileSpec {
  std::vector<std::string> topics{"/a"};
  int count = 3;
  mcap::Timestamp t0 = 1000;
  std::optional<std::string> rosbag2_body;  ///< written as the `rosbag2` record, at open and again at close
  bool zstd_payloads = false;
  std::optional<int> corrupt_index;  ///< this message's frame is truncated
};

FileSpec specWith(std::optional<std::string> rosbag2_body, bool zstd_payloads, mcap::Timestamp t0 = 1000) {
  FileSpec spec;
  spec.rosbag2_body = std::move(rosbag2_body);
  spec.zstd_payloads = zstd_payloads;
  spec.t0 = t0;
  return spec;
}

// Chunked and indexed, so the import takes the parallel indexed path.
void writeFile(const fs::path& path, const FileSpec& spec) {
  mcap::McapWriter writer;
  ASSERT_TRUE(writer.open(PJ::utf8_path::pathToUtf8(path), mcap::McapWriterOptions("ros2")).ok());
  const auto write_record = [&] {
    if (spec.rosbag2_body) {
      mcap::Metadata record;
      record.name = "rosbag2";
      record.metadata["serialized_metadata"] = *spec.rosbag2_body;
      ASSERT_TRUE(writer.write(record).ok());
    }
  };
  write_record();
  mcap::Schema schema("std_msgs/msg/Int32", "ros2msg", "int32 data");
  writer.addSchema(schema);
  std::vector<mcap::ChannelId> ids;
  for (const std::string& topic : spec.topics) {
    mcap::Channel channel(topic, "cdr", schema.id);
    writer.addChannel(channel);
    ids.push_back(channel.id);
  }
  const std::vector<uint8_t> frame = zstdFrame(kCdrInt32);
  std::vector<uint8_t> truncated = frame;
  truncated.resize(truncated.size() - 2);
  for (int i = 0; i < spec.count; ++i) {
    const std::vector<uint8_t>& payload =
        !spec.zstd_payloads ? kCdrInt32 : (spec.corrupt_index == i ? truncated : frame);
    for (mcap::ChannelId id : ids) {
      mcap::Message msg;
      msg.channelId = id;
      msg.logTime = msg.publishTime = spec.t0 + static_cast<mcap::Timestamp>(i);
      msg.data = reinterpret_cast<const std::byte*>(payload.data());
      msg.dataSize = payload.size();
      ASSERT_TRUE(writer.write(msg).ok());
    }
  }
  write_record();
  writer.close();
}

// Probes every fetcher before the fixture sees it, so a failed fetch reaches the plugin with its own error text (the
// fixture replaces it with a generic one). Optionally keeps the first fetcher of one topic alive past the import, as
// the host does for deferred (lazy) pulls.
struct ProbingHost {
  static inline PJ::sdk::testing::DelegatedIngestFixture* fixture = nullptr;
  static inline const PJ_data_source_runtime_host_vtable_t* inner = nullptr;
  static inline std::vector<std::string> fetch_errors;
  static inline std::string retain_topic;
  static inline std::optional<PJ_message_data_fetcher_t> retained;

  static bool pushMessage(
      void* context, PJ_parser_binding_handle_t handle, std::int64_t timestamp_ns, PJ_message_data_fetcher_t fetch,
      PJ_error_t* error) noexcept {
    PJ_payload_t probe{};
    PJ_error_t probe_error{};
    if (!fetch.fetchMessageData(fetch.ctx, &probe, &probe_error)) {
      fetch_errors.emplace_back(probe_error.message);
      if (fetch.release) {
        fetch.release(fetch.ctx);
      }
      if (error != nullptr) {
        *error = probe_error;
      }
      return false;
    }
    if (probe.anchor.release) {
      probe.anchor.release(probe.anchor.ctx);
    }
    if (!retain_topic.empty() && !retained) {
      const auto bindings = fixture->snapshot().bindings;
      if (handle.id > 0 && handle.id <= bindings.size() && bindings[handle.id - 1].topic == retain_topic) {
        retained = fetch;
        fetch.release = nullptr;  // released by the test, after the source is gone
      }
    }
    return inner->push_message(context, handle, timestamp_ns, fetch, error);
  }
};

struct ImportResult {
  bool ok = false;
  std::string error;
  PJ::sdk::testing::DelegatedIngestFixture::Recording recording;
  std::vector<std::string> fetch_errors;
};

// `after_source_destroyed` runs once the data-source handle is gone but the plugin library is still loaded.
ImportResult importPath(
    const fs::path& path, const std::string& retain_topic = {},
    const std::function<void()>& after_source_destroyed = {}) {
  ImportResult result;
  auto library = PJ::DataSourceLibrary::load(std::string(MCAP_SOURCE_PLUGIN_PATH));
  if (!library) {
    result.error = library.error();
    return result;
  }
  PJ::sdk::testing::DelegatedIngestFixture fixture;
  ProbingHost::fixture = &fixture;
  ProbingHost::fetch_errors.clear();
  ProbingHost::retain_topic = retain_topic;
  ProbingHost::retained.reset();
  {
    auto handle = library->createHandle();
    PJ::ServiceRegistryBuilder registry;
    // Bind requires a write host; this delegated-ingest source never writes through it, so an empty vtable is enough.
    static const PJ_source_write_host_vtable_t kNoWrites = [] {
      PJ_source_write_host_vtable_t vtable{};
      vtable.abi_version = PJ_PLUGIN_DATA_API_VERSION;
      vtable.struct_size = sizeof(vtable);
      return vtable;
    }();
    registry.registerService<PJ::sdk::SourceWriteHostService>(
        PJ_source_write_host_t{.ctx = &fixture, .vtable = &kNoWrites});
    PJ_data_source_runtime_host_t runtime_host = fixture.dataSourceView().raw();
    ProbingHost::inner = runtime_host.vtable;
    PJ_data_source_runtime_host_vtable_t probing_vtable = *runtime_host.vtable;
    probing_vtable.push_message = &ProbingHost::pushMessage;
    runtime_host.vtable = &probing_vtable;
    registry.registerService<PJ::sdk::DataSourceRuntimeHostService>(runtime_host);
    if (auto st = handle.bind(registry.view()); !st) {
      result.error = "bind: " + st.error();
      return result;
    }
    if (auto st = handle.loadConfig(R"({"filepath":")" + PJ::utf8_path::utf8String(path.generic_u8string()) + R"("})");
        !st) {
      result.error = "loadConfig: " + st.error();
      return result;
    }
    auto st = handle.start();
    result.ok = static_cast<bool>(st);
    if (!st) {
      result.error = st.error();
    }
    handle.stop();
  }
  result.recording = fixture.snapshot();
  result.fetch_errors = ProbingHost::fetch_errors;
  if (after_source_destroyed) {
    after_source_destroyed();
  }
  if (ProbingHost::retained && ProbingHost::retained->release) {
    ProbingHost::retained->release(ProbingHost::retained->ctx);
  }
  ProbingHost::retained.reset();
  return result;
}

std::map<std::string, std::vector<std::vector<uint8_t>>> payloadsByTopic(const ImportResult& result) {
  std::map<std::string, std::vector<std::vector<uint8_t>>> out;
  for (const auto& push : result.recording.pushes) {
    out[result.recording.bindings.at(push.binding - 1).topic].push_back(push.bytes);
  }
  return out;
}

class MessageCompressionImport : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / (std::string("pj_mcap_message_compression_") +
                                        ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(dir_, ignored);
  }
  void writeSplitYaml(const std::vector<std::string>& files) {
    std::ofstream yaml(dir_ / "metadata.yaml", std::ios::binary);
    yaml << "rosbag2_bagfile_information:\n  storage_identifier: mcap\n  relative_file_paths:\n";
    for (const std::string& file : files) {
      yaml << "    - " << file << "\n";
    }
  }
  fs::path dir_;
};

// Recorded by rosbag2 (Jazzy) with compression_format: zstd, compression_mode: message.
TEST_F(MessageCompressionImport, RealRosbag2BagLoadsWithCdrPayloads) {
  const ImportResult result = importPath(fs::path(MCAP_TEST_DATA_DIR) / "test_rosbag2_message_zstd.mcap");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.fetch_errors.empty()) << ::testing::PrintToString(result.fetch_errors);
  EXPECT_EQ(result.recording.bindings.size(), 2u);

  const auto payloads = payloadsByTopic(result);
  ASSERT_EQ(payloads.at("/odom").size(), 20u);
  ASSERT_EQ(payloads.at("/chatter").size(), 10u);
  for (const auto& [topic, messages] : payloads) {
    for (const auto& bytes : messages) {
      ASSERT_GE(bytes.size(), 4u) << topic;
      EXPECT_EQ(bytes[0], 0x00) << topic;
      EXPECT_EQ(bytes[1], 0x01) << topic;
    }
  }
  // std_msgs/String: CDR header, uint32 length including the terminator, then the characters.
  const auto& chatter = payloads.at("/chatter");
  for (size_t k = 0; k < chatter.size(); ++k) {
    const std::string text = "msg " + std::to_string(2 * k);
    const std::string body(chatter[k].begin() + 8, chatter[k].end());
    EXPECT_EQ(body.substr(0, text.size()), text);
  }
  const std::string odom(payloads.at("/odom").front().begin(), payloads.at("/odom").front().end());
  EXPECT_NE(odom.find("odom"), std::string::npos);
}

// The host keeps fetchers for deferred pulls and invokes them after the source is destroyed: the decoding fetcher
// must not depend on anything of the import.
TEST_F(MessageCompressionImport, DeferredFetchDecodesAfterTheSourceIsGone) {
  std::vector<uint8_t> deferred;
  std::string deferred_error;
  const ImportResult result =
      importPath(fs::path(MCAP_TEST_DATA_DIR) / "test_rosbag2_message_zstd.mcap", "/chatter", [&] {
        ASSERT_TRUE(ProbingHost::retained.has_value());
        PJ_payload_t payload{};
        PJ_error_t error{};
        if (!ProbingHost::retained->fetchMessageData(ProbingHost::retained->ctx, &payload, &error)) {
          deferred_error = error.message;
          return;
        }
        deferred.assign(payload.data, payload.data + payload.size);
        if (payload.anchor.release) {
          payload.anchor.release(payload.anchor.ctx);
        }
      });
  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_TRUE(deferred_error.empty()) << deferred_error;
  ASSERT_GE(deferred.size(), 13u);
  EXPECT_EQ(deferred[0], 0x00);
  EXPECT_EQ(deferred[1], 0x01);
  EXPECT_EQ(std::string(deferred.begin() + 8, deferred.begin() + 13), "msg 0");
  EXPECT_EQ(deferred, payloadsByTopic(result).at("/chatter").front());
}

TEST_F(MessageCompressionImport, CorruptFrameFailsOnlyItsMessage) {
  FileSpec spec = specWith(bareRosbag2Body("zstd", "MESSAGE"), true);
  spec.count = 5;
  spec.corrupt_index = 2;
  writeFile(dir_ / "rec.mcap", spec);

  const ImportResult result = importPath(dir_ / "rec.mcap");
  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.fetch_errors.size(), 1u) << ::testing::PrintToString(result.fetch_errors);
  EXPECT_NE(result.fetch_errors.front().find("per-message zstd decompression failed: "), std::string::npos)
      << result.fetch_errors.front();

  ASSERT_EQ(result.recording.pushes.size(), 4u);
  for (const auto& push : result.recording.pushes) {
    EXPECT_EQ(push.bytes, kCdrInt32);
  }
  bool reported = false;
  for (const auto& note : result.recording.notifications) {
    reported = reported || (note.find("'/a': 1 message(s) could not be loaded") != std::string::npos &&
                            note.find("per-message zstd decompression failed") != std::string::npos);
  }
  EXPECT_TRUE(reported) << ::testing::PrintToString(result.recording.notifications);
}

TEST_F(MessageCompressionImport, UnsupportedFormatFailsBeforePushingAnything) {
  writeFile(dir_ / "rec.mcap", specWith(bareRosbag2Body("lz4", "MESSAGE"), true));

  const ImportResult result = importPath(dir_ / "rec.mcap");
  EXPECT_FALSE(result.ok);
  EXPECT_NE(
      result.error.find(
          "rec.mcap: this ROS 2 bag uses per-message compression 'lz4', which is not supported. Convert it with "
          "'ros2 bag convert' (compression_mode: none)."),
      std::string::npos)
      << result.error;
  EXPECT_TRUE(result.recording.bindings.empty());
  EXPECT_TRUE(result.recording.pushes.empty());
}

// A later split with an unsupported format must not leave the earlier, readable splits half-imported.
TEST_F(MessageCompressionImport, UnsupportedFormatInALaterSplitFailsTheWholeBag) {
  writeFile(dir_ / "bag_0.mcap", specWith(bareRosbag2Body("", ""), false));
  writeFile(dir_ / "bag_1.mcap", specWith(bareRosbag2Body("lz4", "message"), true, 2000));
  writeSplitYaml({"bag_0.mcap", "bag_1.mcap"});

  const ImportResult result = importPath(dir_ / "metadata.yaml");
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("bag_1.mcap: this ROS 2 bag uses per-message compression 'lz4'"), std::string::npos)
      << result.error;
  EXPECT_TRUE(result.recording.pushes.empty());
}

// Each split is read according to its own record, in either order.
TEST_F(MessageCompressionImport, SplitsDecideCompressionIndependently) {
  const FileSpec plain = specWith(bareRosbag2Body("", ""), false);
  const FileSpec compressed = specWith(bareRosbag2Body("zstd", "MESSAGE"), true);
  for (const bool compressed_first : {false, true}) {
    SCOPED_TRACE(compressed_first ? "compressed split first" : "plain split first");
    FileSpec first = compressed_first ? compressed : plain;
    FileSpec second = compressed_first ? plain : compressed;
    second.t0 = 2000;
    writeFile(dir_ / "bag_0.mcap", first);
    writeFile(dir_ / "bag_1.mcap", second);
    writeSplitYaml({"bag_0.mcap", "bag_1.mcap"});

    const ImportResult result = importPath(dir_ / "metadata.yaml");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.fetch_errors.empty()) << ::testing::PrintToString(result.fetch_errors);
    std::vector<int64_t> stamps;
    for (const auto& push : result.recording.pushes) {
      stamps.push_back(push.timestamp_ns);
      EXPECT_EQ(push.bytes, kCdrInt32);
    }
    EXPECT_EQ(stamps, (std::vector<int64_t>{1000, 1001, 1002, 2000, 2001, 2002}));
  }
}

}  // namespace
