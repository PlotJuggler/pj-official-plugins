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

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "data_exporter.hpp"
#include "export_core.hpp"

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

nlohmann::json render(DataExporterDialog& dialog) {
  return nlohmann::json::parse(dialog.widget_data());
}

std::string statusText(DataExporterDialog& dialog) {
  return render(dialog)["statusLabel"]["label"].get<std::string>();
}

#if !defined(_WIN32)
class DirectoryPermissionGuard {
 public:
  explicit DirectoryPermissionGuard(std::filesystem::path path) : path_(std::move(path)) {}

  ~DirectoryPermissionGuard() {
    if (!restored_) {
      (void)::chmod(path_.c_str(), 0700);
    }
  }

  DirectoryPermissionGuard(const DirectoryPermissionGuard&) = delete;
  DirectoryPermissionGuard& operator=(const DirectoryPermissionGuard&) = delete;

  int restore() {
    const int result = ::chmod(path_.c_str(), 0700);
    restored_ = (result == 0);
    return result;
  }

 private:
  std::filesystem::path path_;
  bool restored_ = false;
};
#endif

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

  void setCatalogAvailable(bool available) {
    catalog_available_ = available;
  }

 private:
  static bool acquireCatalogSnapshot(void* context, PJ_catalog_snapshot_t* output, PJ_error_t* error) noexcept {
    auto* self = static_cast<CanonicalStoreReadHost*>(context);
    if (!self->catalog_available_) {
      return false;
    }
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
  bool catalog_available_ = true;
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
  // The test store's catalog exposes no "[stream] "-prefixed data source, so
  // the streaming refresh affordance must stay disabled through a bound host.
  EXPECT_FALSE(nlohmann::json::parse(dialog.widget_data())["refreshButton"]["enabled"].get<bool>());

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

TEST(DataExporterHostTest, UnwritableDestinationReportsFailureWithoutAccepting) {
#if defined(_WIN32)
  GTEST_SKIP() << "This permission-mode test requires POSIX chmod semantics.";
#else
  if (::geteuid() == 0) {
    GTEST_SKIP() << "root bypasses directory write permission checks";
  }

  PJ::testing::ToolboxTestStore store;
  store.addTopic("topic").addField("topic", "value", {0, 1'000'000'000LL}, {1.0, 2.0});
  DataExporterToolbox toolbox;
  CanonicalStoreReadHost canonical_host(store.makeHost());
  PJ::ServiceRegistryBuilder registry;
  bindStore(toolbox, store, canonical_host, registry);
  DataExporterDialog& dialog = toolbox.dialog();
  ASSERT_TRUE(dialog.onClicked("buttonAddAllFiles"));

  TempDirectory temp_directory;
  const auto unwritable_directory = temp_directory.path() / "unwritable";
  ASSERT_TRUE(std::filesystem::create_directory(unwritable_directory));
  if (::chmod(unwritable_directory.c_str(), 0000) != 0) {
    GTEST_SKIP() << "could not remove permissions from " << unwritable_directory;
  }
  DirectoryPermissionGuard permission_guard(unwritable_directory);
  const auto output_path = unwritable_directory / "blocked.csv";

  ASSERT_TRUE(dialog.onFileSelected("saveButton", output_path.string()));
  ASSERT_EQ(permission_guard.restore(), 0);
  const auto state = render(dialog);
  EXPECT_EQ(state["statusLabel"]["label"].get<std::string>(), "Failed to write the output file.");
  EXPECT_FALSE(state.contains("__request_accept"));
  EXPECT_FALSE(std::filesystem::exists(output_path));
#endif
}

TEST(DataExporterHostTest, SecondMultiFileFailureKeepsFirstFileAndAborts) {
  PJ::testing::ToolboxTestStore store;
  store.addTopic("alpha")
      .addField("alpha", "value", {0, 1'000'000'000LL}, {1.0, 2.0})
      .addTopic("zulu")
      .addField("zulu", "value", {0, 1'000'000'000LL}, {3.0, 4.0});
  DataExporterToolbox toolbox;
  CanonicalStoreReadHost canonical_host(store.makeHost());
  PJ::ServiceRegistryBuilder registry;
  bindStore(toolbox, store, canonical_host, registry);
  DataExporterDialog& dialog = toolbox.dialog();
  ASSERT_TRUE(dialog.onClicked("buttonAddAllFiles"));
  ASSERT_TRUE(dialog.onToggled("checkBoxMultifile", true));
  EXPECT_FALSE(dialog.onTextChanged("lineEditPrefix", "partial"));

  TempDirectory temp_directory;
  const auto first_path = temp_directory.path() / "partial_alpha.csv";
  const auto failing_path = temp_directory.path() / "partial_zulu.csv";
  ASSERT_TRUE(std::filesystem::create_directory(failing_path));
  ASSERT_TRUE(dialog.onFolderSelected("saveButton", temp_directory.path().string()));

  const auto state = render(dialog);
  EXPECT_TRUE(std::filesystem::is_regular_file(first_path));
  EXPECT_TRUE(std::filesystem::is_directory(failing_path));
  EXPECT_EQ(state["statusLabel"]["label"].get<std::string>(), "Failed to write file: " + failing_path.string());
  EXPECT_FALSE(state.contains("__request_accept"));
  EXPECT_EQ(state["statusLabel"]["label"].get<std::string>().find("Files saved"), std::string::npos);
}

TEST(DataExporterHostTest, UnresolvableFieldPathReportsNoSamples) {
  PJ::testing::ToolboxTestStore store;
  store.addTopic("known").addField("known", "value", {0}, {1.0});
  DataExporterToolbox toolbox;
  CanonicalStoreReadHost canonical_host(store.makeHost());
  PJ::ServiceRegistryBuilder registry;
  bindStore(toolbox, store, canonical_host, registry);
  DataExporterDialog& dialog = toolbox.dialog();
  ASSERT_TRUE(dialog.onItemsDropped("tableWidget", {"never_registered/value"}));

  TempDirectory temp_directory;
  const auto output_path = temp_directory.path() / "stale.csv";
  ASSERT_TRUE(dialog.onFileSelected("saveButton", output_path.string()));

  const auto state = render(dialog);
  EXPECT_EQ(state["statusLabel"]["label"].get<std::string>(), "No samples found in the selected time range.");
  EXPECT_FALSE(state.contains("__request_accept"));
  EXPECT_FALSE(std::filesystem::exists(output_path));
}

TEST(DataExporterHostTest, LateLoadedCurvesRefreshOnDropAndBothExportPaths) {
  PJ::testing::ToolboxTestStore store;
  DataExporterToolbox toolbox;
  CanonicalStoreReadHost canonical_host(store.makeHost());
  PJ::ServiceRegistryBuilder registry;
  bindStore(toolbox, store, canonical_host, registry);
  DataExporterDialog& dialog = toolbox.dialog();

  store.addTopic("alpha").addField("alpha", "value", {5'000'000'000LL, 7'000'000'000LL}, {1.0, 2.0});
  ASSERT_TRUE(dialog.onItemsDropped("tableWidget", {"alpha/value"}));
  EXPECT_EQ(render(dialog)["rangeSlider"]["range_upper"].get<int>(), 2000);

  ASSERT_TRUE(dialog.onItemsDropped("tableWidget", {"beta/value"}));
  store.addTopic("beta").addField("beta", "value", {5'000'000'000LL, 7'000'000'000LL}, {3.0, 4.0});

  TempDirectory temp_directory;
  const auto single_path = temp_directory.path() / "late_single.csv";
  ASSERT_TRUE(dialog.onFileSelected("saveButton", single_path.string()));
  EXPECT_EQ(
      readFile(single_path),
      "time,alpha/value,beta/value\n"
      "5.000000,1,3\n"
      "7.000000,2,4\n");
  dialog.onAccepted("{}");

  ASSERT_TRUE(dialog.onItemsDropped("tableWidget", {"gamma/value"}));
  store.addTopic("gamma").addField("gamma", "value", {5'000'000'000LL, 7'000'000'000LL}, {5.0, 6.0});
  ASSERT_TRUE(dialog.onToggled("checkBoxMultifile", true));
  EXPECT_FALSE(dialog.onTextChanged("lineEditPrefix", "late"));
  ASSERT_TRUE(dialog.onFolderSelected("saveButton", temp_directory.path().string()));

  const auto gamma_path = temp_directory.path() / "late_gamma.csv";
  ASSERT_TRUE(std::filesystem::is_regular_file(gamma_path)) << statusText(dialog);
  EXPECT_EQ(readFile(gamma_path), "time,value\n5.000000,5\n7.000000,6\n");
}

TEST(DataExporterHostTest, FailedCatalogRefreshRetainsLastSuccessfulSnapshot) {
  PJ::testing::ToolboxTestStore store;
  store.addTopic("topic").addField("topic", "value", {0, 1'000'000'000LL}, {1.0, 2.0});
  DataExporterToolbox toolbox;
  CanonicalStoreReadHost canonical_host(store.makeHost());
  PJ::ServiceRegistryBuilder registry;
  bindStore(toolbox, store, canonical_host, registry);
  DataExporterDialog& dialog = toolbox.dialog();
  ASSERT_TRUE(dialog.onClicked("buttonAddAllFiles"));

  canonical_host.setCatalogAvailable(false);
  ASSERT_TRUE(dialog.onItemsDropped("tableWidget", {"topic/value"}));
  TempDirectory temp_directory;
  const auto output_path = temp_directory.path() / "retained.csv";
  ASSERT_TRUE(dialog.onFileSelected("saveButton", output_path.string()));

  ASSERT_TRUE(std::filesystem::is_regular_file(output_path)) << statusText(dialog);
  EXPECT_EQ(readFile(output_path), "time,topic/value\n0.000000,1\n1.000000,2\n");
}

TEST(DataExporterHostTest, UnicodeFilenameIsPreservedAndContainsValidCsv) {
  PJ::testing::ToolboxTestStore store;
  store.addTopic("topic").addField("topic", "value", {0, 1'000'000'000LL}, {1.25, 2.5});
  DataExporterToolbox toolbox;
  CanonicalStoreReadHost canonical_host(store.makeHost());
  PJ::ServiceRegistryBuilder registry;
  bindStore(toolbox, store, canonical_host, registry);
  DataExporterDialog& dialog = toolbox.dialog();
  ASSERT_TRUE(dialog.onClicked("buttonAddAllFiles"));

  TempDirectory temp_directory;
  const std::string unicode_filename = "übung_日本語.csv";
  const auto output_path = temp_directory.path() / pathFromUtf8(unicode_filename);
  ASSERT_TRUE(dialog.onFileSelected("saveButton", pathToUtf8(output_path)));

  ASSERT_TRUE(std::filesystem::is_regular_file(output_path)) << statusText(dialog);
  EXPECT_EQ(pathToUtf8(output_path.filename()), unicode_filename);
  EXPECT_EQ(
      readFile(output_path),
      "time,topic/value\n"
      "0.000000,1.25\n"
      "1.000000,2.5\n");
  const auto state = render(dialog);
  EXPECT_TRUE(state.value("__request_accept", false));
  EXPECT_EQ(state.value("__request_close", ""), "export_complete");
}

TEST(DataExporterHostTest, EmptyMultiFileGroupIsSkippedWhilePopulatedGroupSucceeds) {
  PJ::testing::ToolboxTestStore store;
  store.addTopic("alpha")
      .addField("alpha", "value", {0, 1'000'000'000LL}, {1.0, 2.0})
      .addTopic("zulu")
      .addField("zulu", "value", {10'000'000'000LL, 11'000'000'000LL}, {3.0, 4.0});
  DataExporterToolbox toolbox;
  CanonicalStoreReadHost canonical_host(store.makeHost());
  PJ::ServiceRegistryBuilder registry;
  bindStore(toolbox, store, canonical_host, registry);
  DataExporterDialog& dialog = toolbox.dialog();
  ASSERT_TRUE(dialog.onClicked("buttonAddAllFiles"));
  ASSERT_TRUE(dialog.onRangeChanged("rangeSlider", 0, 1000));
  ASSERT_TRUE(dialog.onToggled("checkBoxMultifile", true));
  EXPECT_FALSE(dialog.onTextChanged("lineEditPrefix", "range"));

  TempDirectory temp_directory;
  ASSERT_TRUE(dialog.onFolderSelected("saveButton", temp_directory.path().string()));
  const auto populated_path = temp_directory.path() / "range_alpha.csv";
  const auto empty_path = temp_directory.path() / "range_zulu.csv";

  ASSERT_TRUE(std::filesystem::is_regular_file(populated_path)) << statusText(dialog);
  EXPECT_FALSE(std::filesystem::exists(empty_path));
  const auto state = render(dialog);
  EXPECT_TRUE(state.value("__request_accept", false));
  const std::string status = state["statusLabel"]["label"].get<std::string>();
  EXPECT_NE(status.find("range_alpha.csv"), std::string::npos);
  EXPECT_EQ(status.find("range_zulu.csv"), std::string::npos);
}

// ToolboxTestStore intentionally serves float64 only and has no validity or
// UTF-8 population API. M3's Arrow-reader suite therefore remains the direct
// string/null integration coverage (including sliced arrays and embedded NULs),
// while this bound-host test covers the production toolbox orchestration.

}  // namespace
