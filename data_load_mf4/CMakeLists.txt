find_package(nlohmann_json REQUIRED)
find_package(ZLIB REQUIRED)
find_package(EXPAT REQUIRED)

# Conan's CMakeDeps may expose these via config mode under lowercase target
# names (expat::expat / zlib::zlib) rather than the upstream module-mode
# EXPAT::EXPAT / ZLIB::ZLIB that mdflib and this file reference. Normalize.
if(NOT TARGET EXPAT::EXPAT AND TARGET expat::expat)
  add_library(EXPAT::EXPAT ALIAS expat::expat)
endif()
if(NOT TARGET ZLIB::ZLIB AND TARGET zlib::zlib)
  add_library(ZLIB::ZLIB ALIAS zlib::zlib)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

# ---------------------------------------------------------------------------
# mdflib (ihedvall/mdflib, MIT) via CPM — DOWNLOAD_ONLY.
#
# We build the `mdf` static library ourselves from its globbed sources instead
# of add_subdirectory'ing mdflib's own CMake, for two verified reasons (Phase 0):
#   1. mdflib's CMakeLists uses ${CMAKE_SOURCE_DIR} for script/{zlib,expat}.cmake
#      and its include dir — under add_subdirectory that resolves to the PARENT
#      project and fails hard.
#   2. mdflib does not compile on gcc 15: its public header idatawriter.h
#      (std::reverse) and sr4block.h (std::copy_n) rely on transitive
#      <algorithm>/<cstdint> that gcc 15 dropped — force-included below.
# Pinned to the exact commit validated in Phase 0 (untagged upstream HEAD).
# ---------------------------------------------------------------------------
CPMAddPackage(
  NAME mdflib
  URL https://github.com/ihedvall/mdflib/archive/5ad85af2c943a1ab810d0768d3873246a3920ebf.tar.gz
  URL_HASH SHA256=700e11196d570cb480cddc0258d7a1e74adc0562f2d2dfb3ee60f124f79c8cd4
  DOWNLOAD_ONLY YES
)

file(GLOB _mdf_sources CONFIGURE_DEPENDS ${mdflib_SOURCE_DIR}/mdflib/src/*.cpp)
add_library(mdf STATIC ${_mdf_sources})
set_target_properties(mdf PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(mdf PRIVATE cxx_std_17)
# SYSTEM so mdflib's headers don't trip the plugin's -Werror.
target_include_directories(mdf SYSTEM PUBLIC ${mdflib_SOURCE_DIR}/include)
target_include_directories(mdf PRIVATE ${mdflib_SOURCE_DIR}/mdflib/src)
target_link_libraries(mdf PRIVATE ZLIB::ZLIB EXPAT::EXPAT)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # gcc-15 transitive-include workaround (see note above).
  target_compile_options(mdf PRIVATE "SHELL:-include algorithm" "SHELL:-include cstdint")
endif()
if(MSVC)
  target_compile_definitions(mdf PRIVATE _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS _CRT_SECURE_NO_WARNINGS)
endif()

# The CAN/DBC decoder now lives in the shared common/can_dbc lib (target
# pj_can_dbc), added by the root CMakeLists for its consumers.

# ---------------------------------------------------------------------------
# Internal static lib: mdflib reader logic shared by the plugin .so and the
# test executable. PUBLIC-links the SDK base (Status, PrimitiveType) and mdf so
# consumers inherit their include paths.
# ---------------------------------------------------------------------------
add_library(mf4_core STATIC mf4_reader.cpp)
set_target_properties(mf4_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(mf4_core PRIVATE cxx_std_20)
target_compile_options(mf4_core PRIVATE ${PJ_WARNING_FLAGS})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(mf4_core PRIVATE "SHELL:-include algorithm" "SHELL:-include cstdint")
endif()
target_link_libraries(mf4_core PUBLIC plotjuggler_sdk::base mdf)

# ---------------------------------------------------------------------------
# Plugin shared library
# ---------------------------------------------------------------------------
add_library(mf4_source_plugin SHARED mf4_source.cpp mf4_dialog.cpp)
target_compile_features(mf4_source_plugin PRIVATE cxx_std_20)
target_compile_options(mf4_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Our TUs include mdflib public headers (idatawriter.h) — same gcc-15 fix.
  target_compile_options(mf4_source_plugin PRIVATE "SHELL:-include algorithm" "SHELL:-include cstdint")
endif()
target_link_libraries(mf4_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  mf4_core
  pj_can_dbc
  mdf
  ZLIB::ZLIB
  EXPAT::EXPAT
  nlohmann_json::nlohmann_json)

pj_embed_manifest(mf4_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/mf4_manifest.hpp
  VAR_NAME kMf4Manifest
)

# v4 plugin discovery: sidecar .pjmanifest.json scanned pre-dlopen.
pj_emit_plugin_manifest(mf4_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

pj_embed_ui(mf4_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/mf4_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/mf4_dialog_ui.hpp
  VAR_NAME kMf4DialogUi
)

# ---------------------------------------------------------------------------
# Tests (Phase 1: scaffold link check; real module tests land in Phases 2-4)
# ---------------------------------------------------------------------------
add_executable(mf4_source_test
  tests/mf4_source_test.cpp
  tests/mf4_value_test.cpp
  tests/mf4_reader_test.cpp
  tests/mf4_integration_test.cpp
)
target_compile_features(mf4_source_test PRIVATE cxx_std_20)
target_compile_options(mf4_source_test PRIVATE ${PJ_WARNING_FLAGS})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(mf4_source_test PRIVATE "SHELL:-include algorithm" "SHELL:-include cstdint")
endif()
target_link_libraries(mf4_source_test PRIVATE
  plotjuggler_sdk::base
  mf4_core
  mdf
  ZLIB::ZLIB
  EXPAT::EXPAT
  GTest::gtest_main)
add_test(NAME mf4_source_test COMMAND mf4_source_test)
