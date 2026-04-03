find_package(nlohmann_json REQUIRED)
find_package(mcap REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# --- Plugin shared library ---
add_library(mcap_source_plugin SHARED mcap_source.cpp)
target_compile_features(mcap_source_plugin PRIVATE cxx_std_20)
target_compile_options(mcap_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(mcap_source_plugin PRIVATE
  pj_base pj_dialog_sdk nlohmann_json::nlohmann_json mcap::mcap
)

pj_embed_ui(mcap_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/dialog_mcap.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/dialog_mcap_ui.hpp
  VAR_NAME kDialogMcapUi
)

pj_embed_manifest(mcap_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/mcap_manifest.hpp
  VAR_NAME kMcapManifest
)

# --- Unit test for MCAP helpers ---
add_executable(mcap_helpers_test tests/mcap_helpers_test.cpp)
target_compile_features(mcap_helpers_test PRIVATE cxx_std_20)
target_compile_options(mcap_helpers_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(mcap_helpers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mcap_helpers_test PRIVATE mcap::mcap GTest::gtest_main)
add_test(NAME mcap_helpers_test COMMAND mcap_helpers_test)
