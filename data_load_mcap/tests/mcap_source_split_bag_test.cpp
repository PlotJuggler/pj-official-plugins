// Drives the BUILT mcap_source_plugin through a split rosbag2 bag: the splits
// listed in metadata.yaml must import as one dataset — one binding per topic,
// every split's messages pushed through it.
#define MCAP_IMPLEMENTATION
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
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

// `count` messages per topic, log times starting at `t0`. Chunked, so the
// import takes the parallel indexed path with per-file byte stores.
// `latched_publish`, when set, becomes the publishTime of the first message —
// a latched / static publisher stamped long before the recording.
void writeSplit(
    const fs::path& path, const std::vector<std::string>& topics, int count, mcap::Timestamp t0,
    std::optional<mcap::Timestamp> latched_publish = std::nullopt) {
  mcap::McapWriter writer;
  ASSERT_TRUE(writer.open(PJ::utf8_path::pathToUtf8(path), mcap::McapWriterOptions("ros2")).ok());
  mcap::Schema schema("std_msgs/msg/Int32", "ros2msg", "int32 data");
  writer.addSchema(schema);
  const std::string payload("\x00\x01\x00\x00\x2a\x00\x00\x00", 8);
  std::vector<mcap::ChannelId> ids;
  for (const std::string& topic : topics) {
    mcap::Channel channel(topic, "cdr", schema.id);
    writer.addChannel(channel);
    ids.push_back(channel.id);
  }
  for (int i = 0; i < count; ++i) {
    for (mcap::ChannelId id : ids) {
      mcap::Message msg;
      msg.channelId = id;
      msg.logTime = msg.publishTime = t0 + static_cast<mcap::Timestamp>(i);
      if (i == 0 && latched_publish) {
        msg.publishTime = *latched_publish;
      }
      msg.data = reinterpret_cast<const std::byte*>(payload.data());
      msg.dataSize = payload.size();
      ASSERT_TRUE(writer.write(msg).ok());
    }
  }
  writer.close();
}

struct ImportResult {
  bool ok = false;
  std::string error;
  PJ::sdk::testing::DelegatedIngestFixture::Recording recording;
};

// A host whose parser rejects every message of one topic, as when its payloads do not decode: wraps the
// fixture's push_message and fails the pushes on that topic's binding.
struct RefusingHost {
  static inline PJ::sdk::testing::DelegatedIngestFixture* fixture = nullptr;
  static inline const PJ_data_source_runtime_host_vtable_t* inner = nullptr;
  static inline std::string refused_topic;

  static bool pushMessage(
      void* context, PJ_parser_binding_handle_t handle, std::int64_t timestamp_ns, PJ_message_data_fetcher_t fetch,
      PJ_error_t* error) noexcept {
    const auto bindings = fixture->snapshot().bindings;
    if (handle.id > 0 && handle.id <= bindings.size() && bindings[handle.id - 1].topic == refused_topic) {
      if (fetch.release) {
        fetch.release(fetch.ctx);
      }
      PJ::sdk::fillError(error, 1, "test", "CDR read error: not enough data to decode");
      return false;
    }
    return inner->push_message(context, handle, timestamp_ns, fetch, error);
  }
};

ImportResult importPath(const fs::path& path, const std::string& refused_topic = {}) {
  ImportResult result;
  auto library = PJ::DataSourceLibrary::load(std::string(MCAP_SOURCE_PLUGIN_PATH));
  if (!library) {
    result.error = library.error();
    return result;
  }
  auto handle = library->createHandle();
  PJ::sdk::testing::DelegatedIngestFixture fixture;
  PJ::ServiceRegistryBuilder registry;
  // Bind requires a write host; this delegated-ingest source never writes
  // through it, so an empty vtable is enough.
  static const PJ_source_write_host_vtable_t kNoWrites = [] {
    PJ_source_write_host_vtable_t vtable{};
    vtable.abi_version = PJ_PLUGIN_DATA_API_VERSION;
    vtable.struct_size = sizeof(vtable);
    return vtable;
  }();
  registry.registerService<PJ::sdk::SourceWriteHostService>(
      PJ_source_write_host_t{.ctx = &fixture, .vtable = &kNoWrites});
  PJ_data_source_runtime_host_t runtime_host = fixture.dataSourceView().raw();
  PJ_data_source_runtime_host_vtable_t refusing_vtable{};
  if (!refused_topic.empty()) {
    RefusingHost::fixture = &fixture;
    RefusingHost::inner = runtime_host.vtable;
    RefusingHost::refused_topic = refused_topic;
    refusing_vtable = *runtime_host.vtable;
    refusing_vtable.push_message = &RefusingHost::pushMessage;
    runtime_host.vtable = &refusing_vtable;
  }
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
  result.recording = fixture.snapshot();
  return result;
}

class SplitBagImport : public ::testing::Test {
 protected:
  void SetUp() override {
    // Per-test folder: portable (no POSIX getpid, which MSVC rejects as C4996).
    dir_ = fs::temp_directory_path() /
           (std::string("pj_mcap_split_bag_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ignored;  // best effort: never fail a test on cleanup
    fs::remove_all(dir_, ignored);
  }
  void writeYaml(const std::string& body, const fs::path& folder = {}) {
    std::ofstream((folder.empty() ? dir_ : folder) / "metadata.yaml", std::ios::binary)
        << "rosbag2_bagfile_information:\n"
        << body;
  }
  fs::path dir_;
};

TEST_F(SplitBagImport, SplitsImportAsOneDatasetWithOneBindingPerTopic) {
  writeSplit(dir_ / "bag_0.mcap", {"/a"}, 2, 1000);
  writeSplit(dir_ / "bag_1.mcap", {"/a", "/b"}, 3, 2000);
  writeYaml(
      "  storage_identifier: mcap\n"
      "  relative_file_paths:\n"
      "    - bag_0.mcap\n"
      "    - bag_1.mcap\n");

  const ImportResult result = importPath(dir_ / "metadata.yaml");
  ASSERT_TRUE(result.ok) << result.error;

  const auto& bindings = result.recording.bindings;
  ASSERT_EQ(bindings.size(), 2u) << "a topic present in several splits must bind once";
  std::map<std::string, uint32_t> id_by_topic;
  for (size_t i = 0; i < bindings.size(); ++i) {
    id_by_topic[bindings[i].topic] = static_cast<uint32_t>(i + 1);
  }
  ASSERT_TRUE(id_by_topic.contains("/a") && id_by_topic.contains("/b"));

  std::map<uint32_t, std::vector<int64_t>> stamps;
  for (const auto& push : result.recording.pushes) {
    stamps[push.binding].push_back(push.timestamp_ns);
    EXPECT_EQ(push.bytes.size(), 8u);
  }
  EXPECT_EQ(stamps[id_by_topic["/a"]], (std::vector<int64_t>{1000, 1001, 2000, 2001, 2002}));
  EXPECT_EQ(stamps[id_by_topic["/b"]], (std::vector<int64_t>{2000, 2001, 2002}));

  ASSERT_FALSE(result.recording.progress_starts.empty());
  EXPECT_EQ(result.recording.progress_starts.front().total, 8u);
}

// A recorder stopped right after rolling over leaves an EMPTY trailing split
// whose Statistics carry start/end time 0. It must not widen the recording
// window to 0, or out-of-window (latched) publish stamps stop falling back to
// their logTime for every other split.
TEST_F(SplitBagImport, EmptySplitDoesNotWidenThePublishTimeWindow) {
  writeSplit(dir_ / "bag_0.mcap", {"/a"}, 3, 1000, /*latched_publish=*/5);
  writeSplit(dir_ / "bag_1.mcap", {"/a"}, 0, 0);
  writeYaml(
      "  storage_identifier: mcap\n"
      "  relative_file_paths:\n"
      "    - bag_0.mcap\n"
      "    - bag_1.mcap\n");

  const ImportResult result = importPath(dir_ / "metadata.yaml");
  ASSERT_TRUE(result.ok) << result.error;
  std::vector<int64_t> stamps;
  for (const auto& push : result.recording.pushes) {
    stamps.push_back(push.timestamp_ns);
  }
  EXPECT_EQ(stamps, (std::vector<int64_t>{1000, 1001, 1002}));
}

TEST_F(SplitBagImport, PlainMcapStillImportsOnItsOwn) {
  writeSplit(dir_ / "rec.mcap", {"/a", "/b"}, 3, 2000);

  const ImportResult result = importPath(dir_ / "rec.mcap");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.recording.bindings.size(), 2u);
  EXPECT_EQ(result.recording.pushes.size(), 6u);
}

// The host hands the plugin its paths as UTF-8; on Windows a narrow open reads them in the ANSI code page.
TEST_F(SplitBagImport, PlainMcapInAFolderWithNonAsciiNameImports) {
  const fs::path folder = dir_ / fs::path(u8"utf8_\u043f\u0443\u0442\u044c_\u98db\u884c");
  fs::create_directories(folder);
  const fs::path file = folder / fs::path(u8"\u0437\u0430\u043f\u0438\u0441\u044c_\u8a18\u9332.mcap");
  writeSplit(file, {"/a", "/b"}, 3, 2000);

  const ImportResult result = importPath(file);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.recording.bindings.size(), 2u);
  EXPECT_EQ(result.recording.pushes.size(), 6u);
}

TEST_F(SplitBagImport, SplitBagWithNonAsciiNamesImports) {
  const fs::path folder = dir_ / fs::path(u8"utf8_\u043f\u0443\u0442\u044c_\u98db\u884c");
  fs::create_directories(folder);
  const std::u8string first = u8"\u0447\u0430\u0441\u0442\u044c_0.mcap";
  const std::u8string second = u8"\u90e8\u5206_1.mcap";
  writeSplit(folder / fs::path(first), {"/a"}, 2, 1000);
  writeSplit(folder / fs::path(second), {"/a", "/b"}, 3, 2000);
  writeYaml(
      "  storage_identifier: mcap\n"
      "  relative_file_paths:\n"
      "    - " +
          PJ::utf8_path::utf8String(first) +
          "\n"
          "    - " +
          PJ::utf8_path::utf8String(second) + "\n",
      folder);

  const ImportResult result = importPath(folder / "metadata.yaml");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.recording.bindings.size(), 2u);
  EXPECT_EQ(result.recording.pushes.size(), 8u);
}

TEST_F(SplitBagImport, SqliteBagIsRejectedWithoutPushingAnything) {
  writeYaml(
      "  storage_identifier: sqlite3\n"
      "  relative_file_paths:\n"
      "    - bag_0.db3\n");

  const ImportResult result = importPath(dir_ / "metadata.yaml");
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("sqlite3"), std::string::npos) << result.error;
  EXPECT_TRUE(result.recording.bindings.empty());
  EXPECT_TRUE(result.recording.pushes.empty());
}

// One topic whose every message fails to decode must not take the others down: a high-rate bad topic recorded before a
// good one must not hit the consecutive-failure cap and abort the whole import.
TEST_F(SplitBagImport, UndecodableTopicDoesNotAbortTheOthers) {
  const fs::path merged = dir_ / "rec.mcap";
  {
    // One file, every /bad message logged before the first /good one.
    mcap::McapWriter writer;
    ASSERT_TRUE(writer.open(merged.string(), mcap::McapWriterOptions("ros2")).ok());
    mcap::Schema schema("std_msgs/msg/Int32", "ros2msg", "int32 data");
    writer.addSchema(schema);
    mcap::Channel bad("/bad", "cdr", schema.id);
    writer.addChannel(bad);
    mcap::Channel good("/good", "cdr", schema.id);
    writer.addChannel(good);
    const std::string payload("\x00\x01\x00\x00\x2a\x00\x00\x00", 8);
    auto write = [&](mcap::ChannelId id, mcap::Timestamp t) {
      mcap::Message msg;
      msg.channelId = id;
      msg.logTime = msg.publishTime = t;
      msg.data = reinterpret_cast<const std::byte*>(payload.data());
      msg.dataSize = payload.size();
      ASSERT_TRUE(writer.write(msg).ok());
    };
    for (mcap::Timestamp i = 0; i < 150; ++i) {
      write(bad.id, 1000 + i);
    }
    for (mcap::Timestamp i = 0; i < 10; ++i) {
      write(good.id, 2000 + i);
    }
    writer.close();
  }

  const ImportResult result = importPath(merged, "/bad");
  ASSERT_TRUE(result.ok) << result.error;

  uint32_t good_id = 0;
  for (size_t i = 0; i < result.recording.bindings.size(); ++i) {
    if (result.recording.bindings[i].topic == "/good") {
      good_id = static_cast<uint32_t>(i + 1);
    }
  }
  ASSERT_NE(good_id, 0u);
  size_t good_pushes = 0;
  for (const auto& push : result.recording.pushes) {
    good_pushes += push.binding == good_id ? 1 : 0;
  }
  EXPECT_EQ(good_pushes, 10u);

  std::vector<std::string> about_bad;
  for (const auto& note : result.recording.notifications) {
    if (note.find("/bad") != std::string::npos) {
      about_bad.push_back(note);
    }
  }
  ASSERT_EQ(about_bad.size(), 1u) << ::testing::PrintToString(result.recording.notifications);
  EXPECT_NE(about_bad.front().find("CDR read error"), std::string::npos) << about_bad.front();
}

// Every message failing is still a failed load.
TEST_F(SplitBagImport, ImportWhoseEveryMessageFailsIsAnError) {
  writeSplit(dir_ / "rec.mcap", {"/bad"}, 150, 1000);
  const ImportResult result = importPath(dir_ / "rec.mcap", "/bad");
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("loaded no messages"), std::string::npos) << result.error;
}

}  // namespace
