// Drives the built CSV plugin end to end through a minimal recording host.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/host/data_source_library.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <string>
#include <vector>

namespace {

struct RecordingHost {
  int records = 0;
  std::vector<std::string> prompts;  // "title|message" of every message box, answered with Continue

  static bool ensureTopic(void*, PJ_string_view_t, PJ_topic_handle_t* out, PJ_error_t*) noexcept {
    *out = PJ_topic_handle_t{1};
    return true;
  }
  static bool ensureField(
      void*, PJ_topic_handle_t topic, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t* out,
      PJ_error_t*) noexcept {
    *out = PJ_field_handle_t{topic, 1};
    return true;
  }
  static bool appendBoundRecord(
      void* ctx, PJ_topic_handle_t, int64_t, const PJ_bound_field_value_t*, uint64_t, PJ_error_t*) noexcept {
    ++static_cast<RecordingHost*>(ctx)->records;
    return true;
  }
};

struct ImportOutcome {
  bool ok = false;
  std::string error;
};

/// Writes `csv` to a temp file and imports it with `config` (filepath added) into `host`.
ImportOutcome importCsv(const std::string& csv, std::string config, RecordingHost& host) {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("csv_source_test_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + ".csv");
  std::ofstream(path) << csv;

  static const PJ_source_write_host_vtable_t kWrite = [] {
    PJ_source_write_host_vtable_t vtable{};
    vtable.abi_version = PJ_PLUGIN_DATA_API_VERSION;
    vtable.struct_size = sizeof(vtable);
    vtable.ensure_topic = &RecordingHost::ensureTopic;
    vtable.ensure_field = &RecordingHost::ensureField;
    vtable.append_bound_record = &RecordingHost::appendBoundRecord;
    return vtable;
  }();
  static const PJ_data_source_runtime_host_vtable_t kRuntime = [] {
    PJ_data_source_runtime_host_vtable_t vtable{};
    vtable.protocol_version = 1;
    vtable.struct_size = sizeof(vtable);
    vtable.report_message = [](void*, PJ_data_source_message_level_t, PJ_string_view_t) noexcept {};
    vtable.progress_start = [](void*, PJ_string_view_t, uint64_t, bool, PJ_error_t*) noexcept { return true; };
    vtable.progress_update = [](void*, uint64_t) noexcept { return true; };
    vtable.progress_finish = [](void*) noexcept {};
    vtable.is_stop_requested = [](void*) noexcept { return false; };
    vtable.notify_state = [](void*, PJ_data_source_state_t) noexcept {};
    vtable.show_message_box = [](void* ctx, PJ_message_box_type_t, PJ_string_view_t title, PJ_string_view_t message,
                                 int) noexcept -> int {
      static_cast<RecordingHost*>(ctx)->prompts.push_back(
          std::string(title.data, title.size) + "|" + std::string(message.data, message.size));
      return PJ_MSG_BTN_CONTINUE;
    };
    return vtable;
  }();

  ImportOutcome outcome;
  auto library = PJ::DataSourceLibrary::load(std::string(CSV_SOURCE_PLUGIN_PATH));
  if (!library) {
    outcome.error = "load: " + library.error();
    return outcome;
  }
  auto handle = library->createHandle();
  PJ::ServiceRegistryBuilder registry;
  registry.registerService<PJ::sdk::SourceWriteHostService>(PJ_source_write_host_t{.ctx = &host, .vtable = &kWrite});
  registry.registerService<PJ::sdk::DataSourceRuntimeHostService>(
      PJ_data_source_runtime_host_t{.ctx = &host, .vtable = &kRuntime});
  config.insert(1, R"("filepath":")" + path.generic_string() + R"(",)");
  if (auto status = handle.bind(registry.view()); !status) {
    outcome.error = "bind: " + status.error();
  } else if (status = handle.loadConfig(config); !status) {
    outcome.error = "loadConfig: " + status.error();
  } else if (status = handle.start(); !status) {
    outcome.error = status.error();
  } else {
    outcome.ok = true;
  }
  std::filesystem::remove(path);
  return outcome;
}

// QA repro: a custom time format no row matches. Reporting success with zero rows
// made the host commit an empty dataset over the one being reloaded.
TEST(CsvSourceTest, ImportWithEveryRowSkippedFails) {
  RecordingHost host;
  const auto outcome = importCsv(
      "time,a\n0.00,1\n0.01,2\n",
      R"({"time_mode":"column","time_column_index":0,"use_custom_format":true,)"
      R"("custom_time_format":"yyyy-MM-dd hh:mm:ss"})",
      host);
  EXPECT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("Invalid timestamp"), std::string::npos) << outcome.error;
  EXPECT_EQ(host.records, 0);
}

TEST(CsvSourceTest, RowsSkippedPromptDoesNotBlameColumnCountForBadTimestamps) {
  RecordingHost host;
  const auto outcome =
      importCsv("time,a\n0.0,1\nnot_a_time,2\n0.2,3\n", R"({"time_mode":"column","time_column_index":0})", host);
  ASSERT_TRUE(outcome.ok) << outcome.error;
  EXPECT_EQ(host.records, 2);
  ASSERT_EQ(host.prompts.size(), 1u);
  EXPECT_NE(host.prompts[0].find("Invalid timestamp"), std::string::npos) << host.prompts[0];
  EXPECT_EQ(host.prompts[0].find("incorrect number of columns"), std::string::npos) << host.prompts[0];
}

}  // namespace
