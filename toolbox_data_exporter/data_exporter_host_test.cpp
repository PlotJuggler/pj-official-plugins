#include <arrow/api.h>
#include <arrow/io/api.h>
#include <gtest/gtest.h>
#include <parquet/arrow/reader.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>
#include <pj_plugins/testing/toolbox_test_store.hpp>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "data_exporter.hpp"

namespace {

class TempDirectory {
 public:
  TempDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("data_exporter_host_test_" + std::to_string(nonce));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

// SDK 0.18's ToolboxTestStore exports its root StructArray with n_buffers=0.
// MaterializedSeriesView accepts that fixture, but Arrow's canonical C-data
// importer (used by the required validity-aware reader) correctly requires the
// struct validity slot: n_buffers=1 with a null buffer. Forward every catalog
// and read call to the real store and repair only that missing fixture slot.
class CanonicalStoreReadHost {
 public:
  explicit CanonicalStoreReadHost(PJ_toolbox_host_t store_host) : store_host_(store_host) {
    vtable_ = *store_host_.vtable;
    vtable_.acquire_catalog_snapshot = &acquireCatalogSnapshot;
    vtable_.read_series_arrow = &readSeriesArrow;
  }

  [[nodiscard]] PJ_toolbox_host_t makeHost() {
    return PJ_toolbox_host_t{.ctx = this, .vtable = &vtable_};
  }

 private:
  static bool acquireCatalogSnapshot(void* context, PJ_catalog_snapshot_t* output, PJ_error_t* error) noexcept {
    auto* self = static_cast<CanonicalStoreReadHost*>(context);
    return self->store_host_.vtable->acquire_catalog_snapshot(self->store_host_.ctx, output, error);
  }

  static bool readSeriesArrow(
      void* context, PJ_field_handle_t field, ArrowSchema* schema, ArrowArray* array, PJ_error_t* error) noexcept {
    auto* self = static_cast<CanonicalStoreReadHost*>(context);
    if (!self->store_host_.vtable->read_series_arrow(self->store_host_.ctx, field, schema, array, error)) {
      return false;
    }
    if (array->n_buffers == 0 && array->buffers == nullptr) {
      static const void* root_buffers[] = {nullptr};
      array->n_buffers = 1;
      array->buffers = root_buffers;
    }
    return true;
  }

  PJ_toolbox_host_t store_host_{};
  PJ_toolbox_host_vtable_t vtable_{};
};

void bindStore(
    DataExporterToolbox& toolbox, PJ::testing::ToolboxTestStore& store, CanonicalStoreReadHost& canonical_host,
    PJ::ServiceRegistryBuilder& registry) {
  registry.registerService<PJ::sdk::ToolboxHostService>(canonical_host.makeHost());
  registry.registerService<PJ::sdk::ToolboxRuntimeHostService>(store.makeRuntimeHost());
  ASSERT_TRUE(toolbox.bind(PJ::sdk::ServiceRegistry(registry.view())));
  toolbox.prepareDialog();
}

TEST(DataExporterHostTest, AddAllExportsMergedSinglePerTopicMultiAndParquet) {
  PJ::testing::ToolboxTestStore store;
  store.addTopic("topic_a")
      .addField("topic_a", "value", {0, 2'000'000'000LL}, {10.0, 30.0})
      .addTopic("topic_b")
      .addField("topic_b", "value", {1'000'000'000LL, 3'000'000'000LL}, {20.0, 40.0});

  DataExporterToolbox toolbox;
  CanonicalStoreReadHost canonical_host(store.makeHost());
  PJ::ServiceRegistryBuilder registry;
  bindStore(toolbox, store, canonical_host, registry);
  DataExporterDialog& dialog = toolbox.dialog();
  ASSERT_TRUE(dialog.onClicked("buttonAddAllFiles"));
  EXPECT_EQ(dialog.topics(), std::vector<std::string>({"topic_a/value", "topic_b/value"}));

  TempDirectory temp_directory;
  const auto single_base = temp_directory.path() / "merged";
  ASSERT_TRUE(dialog.onFileSelected("saveButton", single_base.string()));
  const auto single_csv = temp_directory.path() / "merged.csv";
  ASSERT_TRUE(std::filesystem::exists(single_csv))
      << nlohmann::json::parse(dialog.widget_data())["statusLabel"]["label"].get<std::string>();
  EXPECT_EQ(
      readFile(single_csv),
      "time,topic_a/value,topic_b/value\n"
      "0.000000,10,20\n"
      "2.000000,30,40\n");
  dialog.onAccepted("{}");

  ASSERT_TRUE(dialog.onToggled("checkBoxMultifile", true));
  EXPECT_FALSE(dialog.onTextChanged("lineEditPrefix", "prefix"));
  ASSERT_TRUE(dialog.onFolderSelected("saveButton", temp_directory.path().string()));
  const auto topic_a_csv = temp_directory.path() / "prefix_topic_a.csv";
  const auto topic_b_csv = temp_directory.path() / "prefix_topic_b.csv";
  ASSERT_TRUE(std::filesystem::exists(topic_a_csv));
  ASSERT_TRUE(std::filesystem::exists(topic_b_csv));
  EXPECT_EQ(readFile(topic_a_csv), "time,value\n0.000000,10\n2.000000,30\n");
  EXPECT_EQ(readFile(topic_b_csv), "time,value\n1.000000,20\n3.000000,40\n");
  const std::string multi_status =
      nlohmann::json::parse(dialog.widget_data())["statusLabel"]["label"].get<std::string>();
  ASSERT_NE(multi_status.find("prefix_topic_a.csv"), std::string::npos);
  ASSERT_NE(multi_status.find("prefix_topic_b.csv"), std::string::npos);
  EXPECT_LT(multi_status.find("prefix_topic_a.csv"), multi_status.find("prefix_topic_b.csv"));
  dialog.onAccepted("{}");

  ASSERT_TRUE(dialog.onToggled("checkBoxMultifile", false));
  ASSERT_TRUE(dialog.onToggled("parquetButton", true));
  const auto parquet_base = temp_directory.path() / "merged_parquet";
  ASSERT_TRUE(dialog.onFileSelected("saveButton", parquet_base.string()));
  const auto parquet_path = temp_directory.path() / "merged_parquet.parquet";
  ASSERT_TRUE(std::filesystem::exists(parquet_path));

  auto input = arrow::io::ReadableFile::Open(parquet_path.string());
  ASSERT_TRUE(input.ok()) << input.status().ToString();
  auto reader_result = parquet::arrow::OpenFile(*input, arrow::default_memory_pool());
  ASSERT_TRUE(reader_result.ok()) << reader_result.status().ToString();
  std::unique_ptr<parquet::arrow::FileReader> reader = std::move(*reader_result);
  std::shared_ptr<arrow::Table> table;
  const auto read_status = reader->ReadTable(&table);
  ASSERT_TRUE(read_status.ok()) << read_status.ToString();
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->num_rows(), 2);
  EXPECT_EQ(table->num_columns(), 3);
}

// ToolboxTestStore intentionally serves float64 only and has no validity or
// UTF-8 population API. M3's Arrow-reader suite therefore remains the direct
// string/null integration coverage (including sliced arrays and embedded NULs),
// while this bound-host test covers the production toolbox orchestration.

}  // namespace
