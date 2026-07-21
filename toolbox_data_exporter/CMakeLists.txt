find_package(nlohmann_json REQUIRED)
if(NOT TARGET Arrow::arrow_static)
  find_package(Arrow REQUIRED)
endif()
if(NOT TARGET Parquet::parquet_static)
  find_package(Parquet REQUIRED)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(toolbox_data_exporter_plugin SHARED data_exporter.cpp export_core.cpp plugin_entry.cpp)

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

# --- Unit test for the host-free export pipeline ---
add_executable(data_exporter_core_test export_core_test.cpp export_core.cpp)
target_compile_features(data_exporter_core_test PRIVATE cxx_std_20)
target_compile_options(data_exporter_core_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(data_exporter_core_test PRIVATE
  GTest::gtest_main
  Arrow::arrow_static
  Parquet::parquet_static
)
add_test(NAME data_exporter_core_test COMMAND data_exporter_core_test)
