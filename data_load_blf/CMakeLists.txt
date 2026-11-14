find_package(nlohmann_json REQUIRED)
find_package(ZLIB REQUIRED)

# Conan may expose zlib in config mode as zlib::zlib; normalize to ZLIB::ZLIB.
if(NOT TARGET ZLIB::ZLIB AND TARGET zlib::zlib)
  add_library(ZLIB::ZLIB ALIAS zlib::zlib)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

# ---------------------------------------------------------------------------
# lblf (PetStr/lblf, MIT) via CPM — DOWNLOAD_ONLY.
#
# Vector BLF reader (classic CAN + LIN + zlib-compressed LogContainers), built
# by cmake/lblf.cmake. Pinned by commit tarball + SHA256.
# ---------------------------------------------------------------------------
CPMAddPackage(
  NAME lblf
  URL https://github.com/PetStr/lblf/archive/a453f188f67637a75e470c272cc76b475080f778.tar.gz
  URL_HASH SHA256=1ed25162dcab2316d7d8b896ac6d85f9d2f97ca2bfdf40b32165c0e4405046ff
  DOWNLOAD_ONLY YES
)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/lblf.cmake)
pj_add_lblf_library(blf ${lblf_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Internal static lib: BLF -> CanFrame adapter, shared by the plugin + tests.
# blf is PRIVATE so lblf headers stay out of consumers (blf_frames.hpp is
# lblf-free).
# ---------------------------------------------------------------------------
add_library(blf_core STATIC blf_frames.cpp)
set_target_properties(blf_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(blf_core PUBLIC cxx_std_20)
target_compile_options(blf_core PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(blf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(blf_core PUBLIC plotjuggler_sdk::base PRIVATE blf)

# ---------------------------------------------------------------------------
# Plugin shared library
# ---------------------------------------------------------------------------
add_library(blf_source_plugin SHARED blf_source.cpp blf_dialog.cpp)
target_compile_features(blf_source_plugin PRIVATE cxx_std_20)
target_compile_options(blf_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(blf_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  blf_core
  pj_can_dbc
  nlohmann_json::nlohmann_json)

pj_embed_manifest(blf_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/blf_manifest.hpp
  VAR_NAME kBlfManifest
)

pj_emit_plugin_manifest(blf_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

pj_embed_ui(blf_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/blf_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/blf_dialog_ui.hpp
  VAR_NAME kBlfDialogUi
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(blf_source_test
  tests/blf_frames_test.cpp
  tests/blf_dialog_test.cpp
  tests/blf_layout_paths_test.cpp
  blf_dialog.cpp
)
target_compile_features(blf_source_test PRIVATE cxx_std_20)
target_compile_options(blf_source_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(blf_source_test PRIVATE
  BLF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data"
  BLF_SOURCE_PLUGIN_PATH="$<TARGET_FILE:blf_source_plugin>")
# blf_layout_paths_test drives the built plugin.
add_dependencies(blf_source_test blf_source_plugin)
# blf_dialog.cpp includes the headers generated for the plugin target above.
# common/can_dbc/tests: shared test helpers (field_test_helpers.hpp, source_import_harness.hpp).
target_include_directories(blf_source_test PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${CMAKE_CURRENT_LIST_DIR}/../common/can_dbc/tests)
target_link_libraries(blf_source_test PRIVATE
  plotjuggler_sdk::plugin_host
  blf_core
  pj_can_dbc
  nlohmann_json::nlohmann_json
  GTest::gtest_main)
add_test(NAME blf_source_test COMMAND blf_source_test)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(blf_source_plugin
  REQUIRED_EXPORTS PJ_get_data_source_vtable PJ_get_dialog_vtable)
