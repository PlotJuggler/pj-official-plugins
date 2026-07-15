# SPDX-License-Identifier: MIT
find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# Vendored LZF codec (BSD-2-Clause) — C sources, kept out of the C++ -Werror wall.
enable_language(C)
add_library(pj_lzf_d STATIC contrib/liblzf/lzf_d.c)   # decompressor (used by the reader)
set_target_properties(pj_lzf_d PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(pj_lzf_d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/contrib/liblzf)

add_library(pj_lzf_c STATIC contrib/liblzf/lzf_c.c)   # compressor (used only by the test fixture)
set_target_properties(pj_lzf_c PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(pj_lzf_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/contrib/liblzf)

# --- Reader library (pure C++, no host/Qt deps) ---
add_library(pj_3d_readers STATIC
  cloud_common.cpp
  pcd_reader.cpp
  ply_reader.cpp
)
set_target_properties(pj_3d_readers PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(pj_3d_readers PUBLIC cxx_std_20)
target_compile_options(pj_3d_readers PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(pj_3d_readers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pj_3d_readers PUBLIC plotjuggler_sdk::plugin_sdk pj_lzf_d)

# --- Plugin shared library ---
add_library(data_load_3d_plugin SHARED loader_3d.cpp)
target_compile_features(data_load_3d_plugin PRIVATE cxx_std_20)
target_compile_options(data_load_3d_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(data_load_3d_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk pj_3d_readers nlohmann_json::nlohmann_json)

pj_embed_manifest(data_load_3d_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/data_load_3d_manifest.hpp
  VAR_NAME kDataLoad3dManifest)
pj_emit_plugin_manifest(data_load_3d_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json)

# --- Tests ---
add_executable(cloud_common_test tests/cloud_common_test.cpp)
target_compile_features(cloud_common_test PRIVATE cxx_std_20)
target_compile_options(cloud_common_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(cloud_common_test PRIVATE
  pj_3d_readers GTest::gtest_main ${CMAKE_DL_LIBS})
add_test(NAME cloud_common_test COMMAND cloud_common_test)

add_executable(pcd_reader_test tests/pcd_reader_test.cpp)
target_compile_features(pcd_reader_test PRIVATE cxx_std_20)
target_compile_options(pcd_reader_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pcd_reader_test PRIVATE
  pj_3d_readers pj_lzf_c GTest::gtest_main ${CMAKE_DL_LIBS})
add_test(NAME pcd_reader_test COMMAND pcd_reader_test)

add_executable(ply_reader_test tests/ply_reader_test.cpp)
target_compile_features(ply_reader_test PRIVATE cxx_std_20)
target_compile_options(ply_reader_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(ply_reader_test PRIVATE
  pj_3d_readers GTest::gtest_main ${CMAKE_DL_LIBS})
add_test(NAME ply_reader_test COMMAND ply_reader_test)

add_executable(loader_helpers_test tests/loader_helpers_test.cpp)
target_compile_features(loader_helpers_test PRIVATE cxx_std_20)
target_compile_options(loader_helpers_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(loader_helpers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(loader_helpers_test PRIVATE
  plotjuggler_sdk::plugin_sdk GTest::gtest_main ${CMAKE_DL_LIBS})
add_test(NAME loader_helpers_test COMMAND loader_helpers_test)
