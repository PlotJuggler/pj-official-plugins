// Drives the built Parquet plugin end to end through a minimal recording host.
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>

#include <cstdint>
#include <filesystem>
#include <pj_base/sdk/plugin_data_api.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/host/data_source_library.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <string>
#include <vector>

namespace {

struct RecordingHost {
  std::vector<int64_t> timestamps;  // one per appended record, in arrival order

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
  static bool appendRecord(
      void* ctx, PJ_topic_handle_t, int64_t timestamp, const PJ_named_field_value_t*, uint64_t, PJ_error_t*) noexcept {
    static_cast<RecordingHost*>(ctx)->timestamps.push_back(timestamp);
    return true;
  }
};

/// Writes `table` to `path` as Parquet; returns an error message, empty on success.
std::string writeParquet(const arrow::Table& table, const std::filesystem::path& path) {
  auto sink = arrow::io::FileOutputStream::Open(path.string());
  if (!sink.ok()) {
    return "open: " + sink.status().ToString();
  }
  auto status = parquet::arrow::WriteTable(table, arrow::default_memory_pool(), *sink);
  if (status.ok()) {
    status = (*sink)->Close();
  }
  return status.ok() ? std::string{} : "write: " + status.ToString();
}

/// Imports `path` through the built plugin with column 0 as the time axis; returns an error, empty on success.
std::string runImport(const std::filesystem::path& path, RecordingHost& host) {
  static const PJ_source_write_host_vtable_t kWrite = [] {
    PJ_source_write_host_vtable_t vtable{};
    vtable.abi_version = PJ_PLUGIN_DATA_API_VERSION;
    vtable.struct_size = sizeof(vtable);
    vtable.ensure_topic = &RecordingHost::ensureTopic;
    vtable.ensure_field = &RecordingHost::ensureField;
    vtable.append_record = &RecordingHost::appendRecord;
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
    return vtable;
  }();

  auto library = PJ::DataSourceLibrary::load(std::string(PARQUET_SOURCE_PLUGIN_PATH));
  if (!library) {
    return "load: " + library.error();
  }
  auto handle = library->createHandle();
  PJ::ServiceRegistryBuilder registry;
  registry.registerService<PJ::sdk::SourceWriteHostService>(PJ_source_write_host_t{.ctx = &host, .vtable = &kWrite});
  registry.registerService<PJ::sdk::DataSourceRuntimeHostService>(
      PJ_data_source_runtime_host_t{.ctx = &host, .vtable = &kRuntime});
  const std::string config =
      R"({"filepath":")" + path.generic_string() + R"(","time_mode":"column","time_column_index":0})";
  if (auto status = handle.bind(registry.view()); !status) {
    return "bind: " + status.error();
  }
  if (auto status = handle.loadConfig(config); !status) {
    return "loadConfig: " + status.error();
  }
  if (auto status = handle.start(); !status) {
    return "start: " + status.error();
  }
  return {};
}

/// Writes `table` to a temp Parquet file, imports it, and removes the file.
std::string importParquet(const arrow::Table& table, RecordingHost& host) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("parquet_source_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + ".parquet");
  std::string error = writeParquet(table, path);
  if (error.empty()) {
    error = runImport(path, host);
  }
  std::filesystem::remove(path);
  return error;
}

// Issue #371: MATLAB writes a timetable's time column as Parquet TIME(MICROS), read by Arrow as
// time64[us]. Every row used to land at timestamp 0, so the file plotted as a single point.
TEST(ParquetSourceTest, Time64MicroColumnSetsTheTimeAxis) {
  constexpr int64_t kRows = 100;
  std::vector<int64_t> micros;
  std::vector<double> values;
  std::vector<int64_t> expected_nanos;
  for (int64_t i = 0; i < kRows; ++i) {
    micros.push_back(i * 100'000);  // 0.1 s apart
    values.push_back(static_cast<double>(i));
    expected_nanos.push_back(i * 100'000'000);
  }
  arrow::Time64Builder time_builder(arrow::time64(arrow::TimeUnit::MICRO), arrow::default_memory_pool());
  arrow::DoubleBuilder value_builder;
  ASSERT_TRUE(time_builder.AppendValues(micros).ok());
  ASSERT_TRUE(value_builder.AppendValues(values).ok());
  auto time_array = time_builder.Finish().ValueOrDie();
  auto value_array = value_builder.Finish().ValueOrDie();
  auto table = arrow::Table::Make(
      arrow::schema({arrow::field("time", time_array->type()), arrow::field("Var1", value_array->type())}),
      {time_array, value_array});

  RecordingHost host;
  const auto error = importParquet(*table, host);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(host.timestamps, expected_nanos);
}

// Arrow 26 writes zstd pages by default from several writers (here parquet-cpp-arrow 26.0.1: time
// snappy, value zstd). The import used to fail with a bare "failed to create batch reader".
TEST(ParquetSourceTest, ImportsZstdCompressedColumns) {
  RecordingHost host;
  const auto error = runImport(std::filesystem::path(PARQUET_TEST_DATA_DIR) / "zstd_values.parquet", host);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(host.timestamps, (std::vector<int64_t>{1'000'000'000, 2'000'000'000, 3'000'000'000}));
}

// The same file with the value column's codec set to brotli, which this plugin does not build: the
// reported error carries Arrow's reason.
TEST(ParquetSourceTest, UnsupportedCodecErrorNamesArrowsReason) {
  RecordingHost host;
  const auto error = runImport(std::filesystem::path(PARQUET_TEST_DATA_DIR) / "brotli_values.parquet", host);
  EXPECT_NE(error.find("Support for codec 'brotli' not built"), std::string::npos) << error;
}

}  // namespace
