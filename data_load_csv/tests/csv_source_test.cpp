// Drives the built CSV plugin end to end through a minimal recording host.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/host/data_source_library.hpp>
#include <pj_plugins/host/dialog_handle.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <set>
#include <string>
#include <vector>

namespace {

struct RecordingHost {
  int records = 0;
  uint32_t fields = 0;
  std::vector<std::string> prompts;  // "title|message" of every message box, answered with Continue

  static bool ensureTopic(void*, PJ_string_view_t, PJ_topic_handle_t* out, PJ_error_t*) noexcept {
    *out = PJ_topic_handle_t{1};
    return true;
  }
  static bool ensureField(
      void* ctx, PJ_topic_handle_t topic, PJ_string_view_t, PJ_primitive_type_t, PJ_field_handle_t* out,
      PJ_error_t*) noexcept {
    *out = PJ_field_handle_t{topic, ++static_cast<RecordingHost*>(ctx)->fields};
    return true;
  }
  // Refuses a record naming a field twice, as the datastore does.
  static bool appendBoundRecord(
      void* ctx, PJ_topic_handle_t, int64_t, const PJ_bound_field_value_t* values, uint64_t count,
      PJ_error_t* error) noexcept {
    std::set<uint32_t> seen;
    for (uint64_t i = 0; i < count; ++i) {
      if (!seen.insert(values[i].field.id).second) {
        PJ::sdk::fillError(error, 1, "test", "duplicate field id");
        return false;
      }
    }
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

// QA repro: a time column that repeats a value failed the whole load with "duplicate field id".
TEST(CsvSourceTest, RepeatedTimestampsLoadEveryRow) {
  RecordingHost host;
  const auto outcome =
      importCsv("t,a,b\n0,1,10\n0,2,20\n1,3,30\n2,4,40\n", R"({"time_mode":"column","time_column_index":0})", host);
  ASSERT_TRUE(outcome.ok) << outcome.error;
  EXPECT_EQ(host.records, 4);
}

/// A dialog event sent between loading a config and saving it: what the user does in the open dialog.
struct DialogEvent {
  std::string widget;
  std::string json;
};

/// Loads `config` with "filepath" set to a file holding `csv`, optionally sends `event` to the dialog, and returns
/// the config the plugin then saves: what PlotJuggler keeps as the pre-fill for the next file, or in a layout.
nlohmann::json loadAndSave(const std::string& csv, nlohmann::json config, const DialogEvent& event = {}) {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("csv_source_test_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_cfg.csv");
  std::ofstream(path) << csv;
  config["filepath"] = path.generic_string();
  auto library = PJ::DataSourceLibrary::load(std::string(CSV_SOURCE_PLUGIN_PATH));
  EXPECT_TRUE(library) << library.error();
  if (!library) {
    return {};
  }
  auto handle = library->createHandle();
  std::string saved = "{}";
  EXPECT_TRUE(handle.loadConfig(config.dump()));
  if (!event.widget.empty()) {
    auto dialog = PJ::DialogHandle::fromBorrowed(handle.getDialog());
    EXPECT_TRUE(dialog.sendEvent(event.widget, event.json));
  }
  EXPECT_TRUE(handle.saveConfig(saved));
  std::filesystem::remove(path);
  return nlohmann::json::parse(saved);
}

/// The config saved after the user picked column "t" (the second one) of a file.
nlohmann::json pickedTimeColumnT() {
  return loadAndSave("a,t,b\n1,0.0,10\n2,0.1,20\n", {{"time_mode", "column"}, {"time_column_index", 1}});
}

// The pre-fill for the next file carries the time column; a file with the same header gets it back.
TEST(CsvTimeColumnRestore, SameHeaderKeepsTheColumn) {
  const auto next = loadAndSave("a,t,b\n3,0.0,30\n", pickedTimeColumnT());
  EXPECT_EQ(next.value("time_mode", ""), "column");
  EXPECT_EQ(next.value("time_column_index", -1), 1);
}

// QA repro: the column was restored by position, so a file with the columns in another order got "b" as time axis.
TEST(CsvTimeColumnRestore, ReorderedHeaderFindsTheColumnByName) {
  const auto next = loadAndSave("t,b,a\n0.0,30,3\n", pickedTimeColumnT());
  EXPECT_EQ(next.value("time_mode", ""), "column");
  EXPECT_EQ(next.value("time_column_index", -1), 0);
}

// A file without the column picks none and opens with the default time axis (row number).
TEST(CsvTimeColumnRestore, HeaderWithoutTheColumnFallsBackToRowNumber) {
  const auto next = loadAndSave("x,y,z\n1,2,3\n", pickedTimeColumnT());
  EXPECT_EQ(next.value("time_mode", ""), "row_number");
  EXPECT_EQ(next.value("time_column_index", -1), -1);
}

// A config saved before the column name was (an older plugin, an older layout) still restores by index.
TEST(CsvTimeColumnRestore, ConfigWithOnlyAnIndexKeepsTheIndex) {
  const auto saved = pickedTimeColumnT();
  EXPECT_EQ(saved.value("time_mode", ""), "column");
  EXPECT_EQ(saved.value("time_column_index", -1), 1);
}

// The date + time pair of "Multiple Columns" is found by name too.
TEST(CsvTimeColumnRestore, ReorderedHeaderFindsTheDateTimePairByName) {
  const auto first = loadAndSave(
      "date,time,v\n2024-01-15,14:30:25,1\n",
      {{"time_mode", "combined"}, {"combined_date_index", 0}, {"combined_time_index", 1}});
  const auto next = loadAndSave("v,time,date\n1,14:30:25,2024-01-15\n", first);
  EXPECT_EQ(next.value("time_mode", ""), "combined");
  EXPECT_EQ(next.value("combined_date_index", -1), 2);
  EXPECT_EQ(next.value("combined_time_index", -1), 1);
}

// A pair only partly found in the new header is no pair: the mode falls back to "Number of Rows" instead of
// pairing the date with whatever other time column the file has ("clock" is a time-only column here).
TEST(CsvTimeColumnRestore, DateTimePairPartlyFoundFallsBackToRowNumber) {
  const auto first = loadAndSave(
      "date,time,v\n2024-01-15,14:30:25,1\n",
      {{"time_mode", "combined"}, {"combined_date_index", 0}, {"combined_time_index", 1}});
  const auto next = loadAndSave("v,date,clock\n1,2024-01-15,14:30:25\n", first);
  EXPECT_EQ(next.value("time_mode", ""), "row_number");
  EXPECT_EQ(next.value("combined_date_index", -1), -1);
  EXPECT_EQ(next.value("combined_time_index", -1), -1);
}

// Changing the delimiter re-splits the header: the picked column is found again by name ("t" is the first column
// split at the comma and the second one split at the semicolon).
TEST(CsvTimeColumnRestore, DelimiterChangeFindsTheColumnByName) {
  const auto saved = loadAndSave(
      "t,a;t\n0,1;2\n",
      {{"delimiter", ","}, {"detect_delimiter", false}, {"time_mode", "column"}, {"time_column_index", 0}},
      {"comboBox", R"({"current_index": 1})"});  // ";" (semicolon)
  EXPECT_EQ(saved.value("delimiter", ""), ";");
  EXPECT_EQ(saved.value("time_mode", ""), "column");
  EXPECT_EQ(saved.value("time_column_name", ""), "t");
  EXPECT_EQ(saved.value("time_column_index", -1), 1);
}

}  // namespace
