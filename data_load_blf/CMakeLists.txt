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
# Vector BLF reader (classic CAN + LIN + zlib-compressed LogContainers). We build
# a static lib from its two LIBRARY sources only — blf_reader.cc + print.cc.
# object_types.cc and the read*.cc files are example programs that each carry
# their own main(), so they are excluded. Pinned by commit tarball + SHA256.
# ---------------------------------------------------------------------------
CPMAddPackage(
  NAME lblf
  URL https://github.com/PetStr/lblf/archive/a453f188f67637a75e470c272cc76b475080f778.tar.gz
  URL_HASH SHA256=1ed25162dcab2316d7d8b896ac6d85f9d2f97ca2bfdf40b32165c0e4405046ff
  DOWNLOAD_ONLY YES
)
add_library(blf STATIC
  ${lblf_SOURCE_DIR}/blf_reader.cc
  ${lblf_SOURCE_DIR}/print.cc
)
set_target_properties(blf PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(blf PRIVATE cxx_std_20)
# SYSTEM so lblf's headers don't trip the plugin's -Werror.
target_include_directories(blf SYSTEM PUBLIC ${lblf_SOURCE_DIR})
target_link_libraries(blf PRIVATE ZLIB::ZLIB)

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
# Plugin shared library (orchestrator lands in Phase 4)
# ---------------------------------------------------------------------------
add_library(blf_source_plugin SHARED blf_source.cpp)
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

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(blf_source_test tests/blf_frames_test.cpp)
target_compile_features(blf_source_test PRIVATE cxx_std_20)
target_compile_options(blf_source_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(blf_source_test PRIVATE
  BLF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data")
target_link_libraries(blf_source_test PRIVATE blf_core GTest::gtest_main)
add_test(NAME blf_source_test COMMAND blf_source_test)
