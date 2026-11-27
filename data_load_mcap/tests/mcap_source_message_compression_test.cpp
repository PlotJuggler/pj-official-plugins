// Drives the BUILT mcap_source_plugin through ROS 2 bags recorded with `compression_mode: message`: every CDR payload
// is a zstd frame, recognised by its magic number. The synthetic files carry no `rosbag2` Metadata record, as after
// `mcap filter`, so nothing but the payload bytes can tell them apart.
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
#include <vector>

namespace {

namespace fs = std::filesystem;

const std::vector<uint8_t> kCdrInt32{0x00, 0x01, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00};

std::vector<uint8_t> zstdFrame(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out(ZSTD_compressBound(in.size()));
  const size_t written = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3);
  EXPECT_EQ(ZSTD_isError(written), 0u);
  out.resize(written);
  return out;
}

struct FileSpec {
  int count = 3;
  mcap::Timestamp t0 = 1000;
  bool zstd_payloads = true;
  std::optional<int> corrupt_index;  ///< this message's frame is truncated
  std::string encoding = "cdr";
  bool chunked = true;  ///< unchunked files take the serial file-order path
};

void writeFile(const fs::path& path, const FileSpec& spec) {
  mcap::McapWriterOptions options("ros2");
  options.noChunking = !spec.chunked;
  mcap::McapWriter writer;
  ASSERT_TRUE(writer.open(PJ::utf8_path::pathToUtf8(path), options).ok());
  mcap::Schema schema("std_msgs/msg/Int32", "ros2msg", "int32 data");
  writer.addSchema(schema);
  mcap::Channel channel("/a", spec.encoding, schema.id);
  writer.addChannel(channel);
  const std::vector<uint8_t> frame = zstdFrame(kCdrInt32);
  std::vector<uint8_t> truncated = frame;
  truncated.resize(truncated.size() - 2);
  for (int i = 0; i < spec.count; ++i) {
    const std::vector<uint8_t>& payload =
        !spec.zstd_payloads ? kCdrInt32 : (spec.corrupt_index == i ? truncated : frame);
    mcap::Message msg;
    msg.channelId = channel.id;
    msg.logTime = msg.publishTime = spec.t0 + static_cast<mcap::Timestamp>(i);
    msg.data = reinterpret_cast<const std::byte*>(payload.data());
    msg.dataSize = payload.size();
    ASSERT_TRUE(writer.write(msg).ok());
  }
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
  void expectAllPayloadsAreCdr(const FileSpec& spec) {
    writeFile(dir_ / "rec.mcap", spec);
    const ImportResult result = importPath(dir_ / "rec.mcap");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.fetch_errors.empty()) << ::testing::PrintToString(result.fetch_errors);
    ASSERT_EQ(result.recording.pushes.size(), static_cast<size_t>(spec.count));
    for (const auto& push : result.recording.pushes) {
      EXPECT_EQ(push.bytes, kCdrInt32);
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

TEST_F(MessageCompressionImport, IndexedFileWithoutMetadataLoads) {
  expectAllPayloadsAreCdr(FileSpec{});
}

TEST_F(MessageCompressionImport, UnchunkedFileLoads) {
  FileSpec spec;
  spec.chunked = false;
  expectAllPayloadsAreCdr(spec);
}

// Only CDR is recognised: no CDR encapsulation header starts with the zstd magic, while other encodings could.
TEST_F(MessageCompressionImport, NonCdrPayloadIsPassedThroughUnchanged) {
  FileSpec spec;
  spec.count = 1;
  spec.encoding = "json";
  writeFile(dir_ / "rec.mcap", spec);
  const ImportResult result = importPath(dir_ / "rec.mcap");
  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.recording.pushes.size(), 1u);
  EXPECT_EQ(result.recording.pushes.front().bytes, zstdFrame(kCdrInt32));
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
  FileSpec spec;
  spec.count = 5;
  spec.corrupt_index = 2;
  writeFile(dir_ / "rec.mcap", spec);

  const ImportResult result = importPath(dir_ / "rec.mcap");
  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.fetch_errors.size(), 1u) << ::testing::PrintToString(result.fetch_errors);
  EXPECT_NE(result.fetch_errors.front().find("zstd decompression failed: "), std::string::npos)
      << result.fetch_errors.front();

  ASSERT_EQ(result.recording.pushes.size(), 4u);
  for (const auto& push : result.recording.pushes) {
    EXPECT_EQ(push.bytes, kCdrInt32);
  }
  bool reported = false;
  for (const auto& note : result.recording.notifications) {
    reported = reported || (note.find("'/a': 1 message(s) could not be loaded") != std::string::npos &&
                            note.find("zstd decompression failed") != std::string::npos);
  }
  EXPECT_TRUE(reported) << ::testing::PrintToString(result.recording.notifications);
}

// Each split is decided by its own payloads, in either order.
TEST_F(MessageCompressionImport, PlainAndCompressedSplitsLoadTogether) {
  for (const bool compressed_first : {false, true}) {
    SCOPED_TRACE(compressed_first ? "compressed split first" : "plain split first");
    FileSpec first;
    first.zstd_payloads = compressed_first;
    FileSpec second;
    second.t0 = 2000;
    second.zstd_payloads = !compressed_first;
    writeFile(dir_ / "bag_0.mcap", first);
    writeFile(dir_ / "bag_1.mcap", second);
    std::ofstream(dir_ / "metadata.yaml", std::ios::binary)
        << "rosbag2_bagfile_information:\n  storage_identifier: mcap\n  relative_file_paths:\n"
           "    - bag_0.mcap\n    - bag_1.mcap\n";

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
