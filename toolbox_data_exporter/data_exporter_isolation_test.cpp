#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include <pj_base/data_source_protocol.h>
#include <pj_base/toolbox_protocol.h>
#include <pj_plugins/dialog_protocol.h>

#ifndef DATA_EXPORTER_PLUGIN_PATH
#define DATA_EXPORTER_PLUGIN_PATH ""
#endif

#ifndef DATA_LOAD_PARQUET_PLUGIN_PATH
#define DATA_LOAD_PARQUET_PLUGIN_PATH ""
#endif

namespace {

#if !defined(_WIN32)

class SharedLibrary {
 public:
  explicit SharedLibrary(const std::filesystem::path& path) : handle_(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {}

  ~SharedLibrary() {
    if (handle_ != nullptr) {
      dlclose(handle_);
    }
  }

  SharedLibrary(const SharedLibrary&) = delete;
  SharedLibrary& operator=(const SharedLibrary&) = delete;

  [[nodiscard]] void* get() const {
    return handle_;
  }

 private:
  void* handle_ = nullptr;
};

template <typename Function>
Function resolveSymbol(void* library, const char* name) {
  static_assert(sizeof(Function) == sizeof(void*));
  (void)dlerror();
  void* symbol = dlsym(library, name);
  const char* error = dlerror();
  EXPECT_EQ(error, nullptr) << "dlsym(" << name << ") failed: " << (error == nullptr ? "" : error);
  Function function = nullptr;
  if (error == nullptr) {
    std::memcpy(&function, &symbol, sizeof(function));
  }
  return function;
}

TEST(DataExporterIsolationTest, ArrowLinkedPluginsCoexistWithLocalSymbolScopes) {
  const std::filesystem::path exporter_path(DATA_EXPORTER_PLUGIN_PATH);
  const std::filesystem::path parquet_path(DATA_LOAD_PARQUET_PLUGIN_PATH);
  if (!std::filesystem::is_regular_file(exporter_path)) {
    GTEST_SKIP() << "toolbox_data_exporter DSO is missing: " << exporter_path;
  }
  if (!std::filesystem::is_regular_file(parquet_path)) {
    GTEST_SKIP() << "data_load_parquet DSO is missing: " << parquet_path;
  }

  SharedLibrary exporter(exporter_path);
  ASSERT_NE(exporter.get(), nullptr) << "dlopen(" << exporter_path << ") failed: " << dlerror();
  SharedLibrary parquet(parquet_path);
  ASSERT_NE(parquet.get(), nullptr) << "dlopen(" << parquet_path
                                    << ") failed after the exporter was loaded: " << dlerror();

  const auto get_toolbox = resolveSymbol<PJ_get_toolbox_vtable_fn>(exporter.get(), "PJ_get_toolbox_vtable");
  const auto get_exporter_dialog = resolveSymbol<PJ_get_dialog_vtable_fn>(exporter.get(), "PJ_get_dialog_vtable");
  const auto get_data_source = resolveSymbol<PJ_get_data_source_vtable_fn>(parquet.get(), "PJ_get_data_source_vtable");
  const auto get_parquet_dialog = resolveSymbol<PJ_get_dialog_vtable_fn>(parquet.get(), "PJ_get_dialog_vtable");
  ASSERT_NE(get_toolbox, nullptr);
  ASSERT_NE(get_exporter_dialog, nullptr);
  ASSERT_NE(get_data_source, nullptr);
  ASSERT_NE(get_parquet_dialog, nullptr);

  const PJ_toolbox_vtable_t* toolbox_vtable = get_toolbox();
  const PJ_dialog_vtable_t* exporter_dialog_vtable = get_exporter_dialog();
  const PJ_data_source_vtable_t* data_source_vtable = get_data_source();
  const PJ_dialog_vtable_t* parquet_dialog_vtable = get_parquet_dialog();
  ASSERT_NE(toolbox_vtable, nullptr);
  ASSERT_NE(exporter_dialog_vtable, nullptr);
  ASSERT_NE(data_source_vtable, nullptr);
  ASSERT_NE(parquet_dialog_vtable, nullptr);

  EXPECT_EQ(toolbox_vtable->protocol_version, static_cast<uint32_t>(PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION));
  EXPECT_GE(toolbox_vtable->struct_size, static_cast<uint32_t>(PJ_TOOLBOX_MIN_VTABLE_SIZE));
  EXPECT_EQ(data_source_vtable->protocol_version, static_cast<uint32_t>(PJ_DATA_SOURCE_PROTOCOL_VERSION));
  EXPECT_GE(data_source_vtable->struct_size, static_cast<uint32_t>(PJ_DATA_SOURCE_MIN_VTABLE_SIZE));
  EXPECT_EQ(exporter_dialog_vtable->protocol_version, static_cast<uint32_t>(PJ_DIALOG_PROTOCOL_VERSION));
  EXPECT_GE(exporter_dialog_vtable->struct_size, static_cast<uint32_t>(PJ_DIALOG_MIN_VTABLE_SIZE));
  EXPECT_EQ(parquet_dialog_vtable->protocol_version, static_cast<uint32_t>(PJ_DIALOG_PROTOCOL_VERSION));
  EXPECT_GE(parquet_dialog_vtable->struct_size, static_cast<uint32_t>(PJ_DIALOG_MIN_VTABLE_SIZE));

  ASSERT_NE(toolbox_vtable->create, nullptr);
  ASSERT_NE(toolbox_vtable->destroy, nullptr);
  void* toolbox_instance = toolbox_vtable->create();
  ASSERT_NE(toolbox_instance, nullptr);
  toolbox_vtable->destroy(toolbox_instance);

  ASSERT_NE(data_source_vtable->create, nullptr);
  ASSERT_NE(data_source_vtable->destroy, nullptr);
  void* data_source_instance = data_source_vtable->create();
  ASSERT_NE(data_source_instance, nullptr);
  data_source_vtable->destroy(data_source_instance);

  ASSERT_NE(exporter_dialog_vtable->create, nullptr);
  ASSERT_NE(exporter_dialog_vtable->get_ui_content, nullptr);
  ASSERT_NE(exporter_dialog_vtable->destroy, nullptr);
  void* dialog_instance = exporter_dialog_vtable->create();
  ASSERT_NE(dialog_instance, nullptr);
  const char* ui_content = exporter_dialog_vtable->get_ui_content(dialog_instance);
  ASSERT_NE(ui_content, nullptr);
  EXPECT_NE(std::string(ui_content).find("<class>toolBoxUI</class>"), std::string::npos);
  exporter_dialog_vtable->destroy(dialog_instance);
}

#else

TEST(DataExporterIsolationTest, ArrowLinkedPluginsCoexistWithLocalSymbolScopes) {
  GTEST_SKIP() << "This test verifies the POSIX dlopen(RTLD_NOW | RTLD_LOCAL) loading path.";
}

#endif

}  // namespace
