#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/host/data_source_library.hpp>
#include <pj_plugins/host/dialog_handle.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <string>
#include <vector>

namespace {

std::string utf8(const std::filesystem::path& path) {
  const auto bytes = path.u8string();
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string pluginUnderTest() {
#ifdef _WIN32
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, "PJ_CSV_PLUGIN_UNDER_TEST") != 0) {
    return PJ_CSV_PLUGIN_PATH;
  }
  const std::string path = value != nullptr ? value : PJ_CSV_PLUGIN_PATH;
  std::free(value);
  return path;
#else
  const char* value = std::getenv("PJ_CSV_PLUGIN_UNDER_TEST");
  return value != nullptr ? value : PJ_CSV_PLUGIN_PATH;
#endif
}

struct CapturedData {
  std::vector<std::string> fields;
  std::vector<int64_t> timestamps;
  std::vector<std::vector<double>> rows;
};

PJ_source_write_host_t writeHost(CapturedData* data) {
  static const auto vtable = [] {
    PJ_source_write_host_vtable_t vt{};
    vt.abi_version = PJ_PLUGIN_DATA_API_VERSION;
    vt.struct_size = sizeof(vt);
    vt.ensure_topic = +[](void*, PJ_string_view_t, PJ_topic_handle_t* out, PJ_error_t*) noexcept {
      *out = PJ_topic_handle_t{1};
      return true;
    };
    vt.ensure_field = +[](void* ctx, PJ_topic_handle_t topic, PJ_string_view_t name, PJ_primitive_type_t type,
                          PJ_field_handle_t* out, PJ_error_t*) noexcept {
      if (type != PJ_PRIMITIVE_TYPE_FLOAT64) {
        return false;
      }
      auto& fields = static_cast<CapturedData*>(ctx)->fields;
      fields.emplace_back(name.data, name.size);
      *out = PJ_field_handle_t{topic, static_cast<uint32_t>(fields.size())};
      return true;
    };
    vt.append_record = +[](void*, PJ_topic_handle_t, int64_t, const PJ_named_field_value_t*, uint64_t,
                           PJ_error_t*) noexcept { return false; };
    vt.append_bound_record = +[](void* ctx, PJ_topic_handle_t, int64_t timestamp, const PJ_bound_field_value_t* values,
                                 uint64_t count, PJ_error_t*) noexcept {
      auto& state = *static_cast<CapturedData*>(ctx);
      std::vector<double> row(state.fields.size());
      for (uint64_t i = 0; i < count; ++i) {
        const auto& value = values[i];
        if (value.is_null || value.value.type != PJ_PRIMITIVE_TYPE_FLOAT64 || value.field.id == 0 ||
            value.field.id > row.size()) {
          return false;
        }
        row[value.field.id - 1] = value.value.data.as_float64;
      }
      if (count != row.size()) {
        return false;
      }
      state.timestamps.push_back(timestamp);
      state.rows.push_back(std::move(row));
      return true;
    };
    vt.append_arrow_stream =
        +[](void*, PJ_topic_handle_t, ArrowArrayStream*, PJ_string_view_t, PJ_error_t*) noexcept { return false; };
    return vt;
  }();
  return {data, &vtable};
}

PJ_data_source_runtime_host_t runtimeHost(void* context) {
  static const auto vtable = [] {
    PJ_data_source_runtime_host_vtable_t vt{};
    vt.protocol_version = 1;
    vt.struct_size = sizeof(vt);
    vt.report_message = +[](void*, PJ_data_source_message_level_t, PJ_string_view_t) noexcept {};
    vt.progress_start = +[](void*, PJ_string_view_t, uint64_t, bool, PJ_error_t*) noexcept { return true; };
    vt.progress_update = +[](void*, uint64_t) noexcept { return true; };
    vt.progress_finish = +[](void*) noexcept {};
    vt.is_stop_requested = +[](void*) noexcept { return false; };
    vt.notify_state = +[](void*, PJ_data_source_state_t) noexcept {};
    vt.request_stop = +[](void*, PJ_data_source_state_t, PJ_string_view_t) noexcept {};
    vt.show_message_box = +[](void*, PJ_message_box_type_t, PJ_string_view_t, PJ_string_view_t, int) noexcept -> int {
      return PJ_MSG_BTN_OK;
    };
    return vt;
  }();
  return {context, &vtable};
}

struct PathCase {
  const char* name;
  std::u8string relative_path;
};

class CsvFilePaths : public ::testing::TestWithParam<PathCase> {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("pj_csv_paths_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    path_ = root_ / std::filesystem::path(GetParam().relative_path);
    std::filesystem::create_directories(path_.parent_path());
    // Deliberately synthetic, identical bytes in every path case.
    std::ofstream file(path_, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    file << "sample_index,synthetic_signal\n0,-2\n1,3.5\n2,0\n";
    file.close();
    ASSERT_TRUE(file.good());

    const std::string plugin_path = pluginUnderTest();
    auto loaded =
        PJ::DataSourceLibrary::load(std::filesystem::path(std::u8string(plugin_path.begin(), plugin_path.end())));
    ASSERT_TRUE(loaded) << loaded.error();
    library_ = std::move(*loaded);
    registry_.registerService<PJ::sdk::SourceWriteHostService>(writeHost(&data_));
    registry_.registerService<PJ::sdk::DataSourceRuntimeHostService>(runtimeHost(&data_));
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  std::string config(const std::filesystem::path& path) const {
    return nlohmann::json{
        {"filepath", utf8(path)}, {"delimiter", ","}, {"detect_delimiter", false}, {"time_mode", "row_number"}}
        .dump();
  }

  std::filesystem::path root_;
  std::filesystem::path path_;
  PJ::DataSourceLibrary library_;
  CapturedData data_;
  PJ::ServiceRegistryBuilder registry_;
};

TEST_P(CsvFilePaths, PreviewReadsUtf8PathAndPreservesConfig) {
  auto source = library_.createHandle();
  ASSERT_TRUE(source.loadConfig(config(path_)));
  auto dialog = PJ::DialogHandle::fromBorrowed(source.getDialog());
  const auto widgets = nlohmann::json::parse(dialog.widget_data());
  EXPECT_EQ(widgets.at("tableView").at("headers"), nlohmann::json::array({"sample_index", "synthetic_signal"}));
  EXPECT_EQ(widgets.at("tableView").at("rows"), nlohmann::json::array({{"0", "-2"}, {"1", "3.5"}, {"2", "0"}}));
  EXPECT_EQ(nlohmann::json::parse(dialog.save_config()).at("filepath"), utf8(path_));
}

TEST_P(CsvFilePaths, ImportReadsAllSamplesWithUnchangedRowTimeAxis) {
  auto source = library_.createHandle();
  const auto bound = source.bind(registry_.view());
  ASSERT_TRUE(bound) << bound.error();
  ASSERT_TRUE(source.loadConfig(config(path_)));
  const auto result = source.start();
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(data_.fields, (std::vector<std::string>{"sample_index", "synthetic_signal"}));
  EXPECT_EQ(data_.timestamps, (std::vector<int64_t>{0, 1000000000, 2000000000}));
  EXPECT_EQ(data_.rows, (std::vector<std::vector<double>>{{0, -2}, {1, 3.5}, {2, 0}}));
}

TEST_P(CsvFilePaths, MissingFileReturnsFailureWithoutWritingData) {
  auto source = library_.createHandle();
  const auto bound = source.bind(registry_.view());
  ASSERT_TRUE(bound) << bound.error();
  ASSERT_TRUE(source.loadConfig(config(path_.parent_path() / "missing.csv")));
  EXPECT_FALSE(source.start());
  EXPECT_TRUE(data_.fields.empty());
  EXPECT_TRUE(data_.rows.empty());
}

INSTANTIATE_TEST_SUITE_P(
    Utf8Paths, CsvFilePaths,
    ::testing::Values(
        PathCase{"Ascii", u8"ascii/input.csv"}, PathCase{"ChineseDirectory", u8"\u6d4b\u8bd5/input.csv"},
        PathCase{"ChineseFilename", u8"ascii/\u6570\u636e.csv"},
        PathCase{"TraditionalAndEmoji", u8"\u6e2c\u8a66/\u5408\u6210_\U0001f4c8.csv"}),
    [](const ::testing::TestParamInfo<PathCase>& test_case) { return test_case.param.name; });

}  // namespace
