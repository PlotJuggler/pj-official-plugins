find_package(nlohmann_json REQUIRED)
if(NOT TARGET Arrow::arrow_static)
  find_package(Arrow REQUIRED)
endif()
if(NOT TARGET Parquet::parquet_static)
  find_package(Parquet REQUIRED)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(toolbox_data_exporter_plugin SHARED arrow_series_reader.cpp data_exporter.cpp export_core.cpp plugin_entry.cpp)

target_compile_features(toolbox_data_exporter_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_data_exporter_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_data_exporter_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  Arrow::arrow_static
  Parquet::parquet_static
)

target_include_directories(toolbox_data_exporter_plugin PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

pj_embed_ui(toolbox_data_exporter_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/exporter_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/exporter_dialog_ui.hpp
  VAR_NAME kDataExporterDialogUi
)

pj_embed_manifest(toolbox_data_exporter_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/data_exporter_manifest.hpp
  VAR_NAME kDataExporterManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(toolbox_data_exporter_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Shared test library: the production TUs compiled once for all tests ---
# The plugin target above keeps its own compilation of the same sources so the
# pj_emit_plugin_manifest visibility/flags applied to the DSO stay untouched.
add_library(data_exporter_testcore STATIC
  arrow_series_reader.cpp
  data_exporter.cpp
  export_core.cpp
  plugin_entry.cpp
)
target_compile_features(data_exporter_testcore PRIVATE cxx_std_20)
target_compile_options(data_exporter_testcore PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(data_exporter_testcore PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(data_exporter_testcore PUBLIC
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  Arrow::arrow_static
  Parquet::parquet_static
)
# The generated ui/manifest headers are produced by custom commands attached to
# the plugin target; order the shared test compilation after them.
add_dependencies(data_exporter_testcore toolbox_data_exporter_plugin)

# --- Unit test for the host-free export pipeline ---
add_executable(data_exporter_core_test export_core_test.cpp)
target_compile_features(data_exporter_core_test PRIVATE cxx_std_20)
target_compile_options(data_exporter_core_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(data_exporter_core_test PRIVATE
  data_exporter_testcore
  GTest::gtest_main
)
add_test(NAME data_exporter_core_test COMMAND data_exporter_core_test)

# --- Unit test for validity-aware Arrow series decoding ---
add_executable(data_exporter_arrow_reader_test arrow_series_reader_test.cpp)
target_compile_features(data_exporter_arrow_reader_test PRIVATE cxx_std_20)
target_compile_options(data_exporter_arrow_reader_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(data_exporter_arrow_reader_test PRIVATE
  data_exporter_testcore
  GTest::gtest_main
)
add_test(NAME data_exporter_arrow_reader_test COMMAND data_exporter_arrow_reader_test)

# --- Milestone 4 dialog-state + bound-host integration tests ---
# One binary is registered under two filtered ctest names so both required
# targets are discoverable through `ctest -R data_exporter` without compiling
# the Arrow-heavy production translation units twice.
add_executable(
  data_exporter_milestone4_test
  data_exporter_dialog_test.cpp
  data_exporter_host_test.cpp
)
target_compile_features(data_exporter_milestone4_test PRIVATE cxx_std_20)
target_compile_options(data_exporter_milestone4_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(data_exporter_milestone4_test PRIVATE
  data_exporter_testcore
  plotjuggler_sdk::plugin_host
  GTest::gtest_main
)
add_test(
  NAME data_exporter_dialog_test
  COMMAND data_exporter_milestone4_test --gtest_filter=DataExporterDialogTest.*
)
add_test(
  NAME data_exporter_host_test
  COMMAND data_exporter_milestone4_test --gtest_filter=DataExporterHostTest.*
)

# --- Milestone 5 dual-plugin load / static-Arrow symbol-isolation test ---
set(DATA_LOAD_PARQUET_PLUGIN_PATH
  "${CMAKE_SOURCE_DIR}/build/data_load_parquet/${CMAKE_BUILD_TYPE}/bin/${CMAKE_SHARED_LIBRARY_PREFIX}parquet_source_plugin${CMAKE_SHARED_LIBRARY_SUFFIX}"
  CACHE FILEPATH "Path to the separately built data_load_parquet plugin DSO"
)
if(TARGET parquet_source_plugin)
  set(DATA_LOAD_PARQUET_PLUGIN_TEST_PATH "$<TARGET_FILE:parquet_source_plugin>")
else()
  set(DATA_LOAD_PARQUET_PLUGIN_TEST_PATH "${DATA_LOAD_PARQUET_PLUGIN_PATH}")
endif()
add_executable(data_exporter_isolation_test data_exporter_isolation_test.cpp)
target_compile_features(data_exporter_isolation_test PRIVATE cxx_std_20)
target_compile_options(data_exporter_isolation_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(data_exporter_isolation_test PRIVATE
  DATA_EXPORTER_PLUGIN_PATH="$<TARGET_FILE:toolbox_data_exporter_plugin>"
  DATA_LOAD_PARQUET_PLUGIN_PATH="${DATA_LOAD_PARQUET_PLUGIN_TEST_PATH}"
)
target_link_libraries(data_exporter_isolation_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  GTest::gtest_main
  ${CMAKE_DL_LIBS}
)
add_dependencies(data_exporter_isolation_test toolbox_data_exporter_plugin)
if(TARGET parquet_source_plugin)
  add_dependencies(data_exporter_isolation_test parquet_source_plugin)
endif()
add_test(NAME data_exporter_isolation_test COMMAND data_exporter_isolation_test)
# The test GTEST_SKIP()s when the data_load_parquet DSO is absent (single-plugin
# builds); surface that as ***Skipped in ctest instead of a false Passed. The
# property is binary-granular (any skipped gtest marks the whole entry Skipped) —
# acceptable here because the DSO gate skips this binary all-or-nothing.
set_tests_properties(data_exporter_isolation_test PROPERTIES
  SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
)
