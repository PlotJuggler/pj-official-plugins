find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

# ---------------------------------------------------------------------------
# Internal static lib: candump line parsing (log + screen format) and the
# ARUS-CSV -> DBC translator, shared by the plugin + tests. No vendored/CPM
# dependency of its own -- unlike data_load_blf/data_load_mf4 it does not
# parse a binary container format, so there is nothing to vendor here; CAN
# decoding itself is the shared pj_can_dbc library.
# ---------------------------------------------------------------------------
add_library(candump_core STATIC candump_parser.cpp csv_dictionary.cpp)
set_target_properties(candump_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(candump_core PUBLIC cxx_std_20)
target_compile_options(candump_core PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(candump_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# pj_can_dbc: csv_dictionary.cpp reuses its hexId() (message-id hex text) --
# same convention as the plugin's own raw-topic fallback naming.
target_link_libraries(candump_core PUBLIC plotjuggler_sdk::base PRIVATE pj_can_dbc)

# ---------------------------------------------------------------------------
# Plugin shared library
# ---------------------------------------------------------------------------
add_library(candump_source_plugin SHARED candump_source.cpp candump_dialog.cpp)
target_compile_features(candump_source_plugin PRIVATE cxx_std_20)
target_compile_options(candump_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(candump_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  candump_core
  pj_can_dbc
  nlohmann_json::nlohmann_json)

pj_embed_manifest(candump_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/candump_manifest.hpp
  VAR_NAME kCandumpManifest
)

pj_emit_plugin_manifest(candump_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

pj_embed_ui(candump_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/candump_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/candump_dialog_ui.hpp
  VAR_NAME kCandumpDialogUi
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(candump_source_test
  tests/candump_parser_test.cpp
  tests/csv_dictionary_test.cpp
  tests/candump_decode_test.cpp
  tests/candump_dialog_test.cpp
  candump_dialog.cpp
)
target_compile_features(candump_source_test PRIVATE cxx_std_20)
target_compile_options(candump_source_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(candump_source_test PRIVATE
  CANDUMP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data")
# candump_dialog.cpp includes the headers generated for the plugin target above.
target_include_directories(candump_source_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(candump_source_test PRIVATE
  plotjuggler_sdk::plugin_host
  candump_core
  pj_can_dbc
  nlohmann_json::nlohmann_json
  GTest::gtest_main)
add_test(NAME candump_source_test COMMAND candump_source_test)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(candump_source_plugin
  REQUIRED_EXPORTS PJ_get_data_source_vtable PJ_get_dialog_vtable)
