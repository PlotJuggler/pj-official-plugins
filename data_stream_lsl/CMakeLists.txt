find_package(LSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# --- Plugin shared library ---
add_library(lsl_source_plugin SHARED lsl_source.cpp)
target_compile_features(lsl_source_plugin PRIVATE cxx_std_20)
target_compile_options(lsl_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(lsl_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json LSL::lsl
)

pj_embed_ui(lsl_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/datastream_lsl.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/datastream_lsl_ui.hpp
  VAR_NAME kDataStreamLslUi
)

pj_embed_manifest(lsl_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/lsl_manifest.hpp
  VAR_NAME kLslManifest
)

pj_emit_plugin_manifest(lsl_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Unit tests for the pure conversion helpers ---
add_executable(lsl_source_test tests/lsl_source_test.cpp)
target_compile_features(lsl_source_test PRIVATE cxx_std_20)
target_include_directories(lsl_source_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lsl_source_test PRIVATE
  plotjuggler_sdk::plugin_host LSL::lsl nlohmann_json::nlohmann_json GTest::gtest_main
)
add_test(NAME lsl_source_test COMMAND lsl_source_test)
