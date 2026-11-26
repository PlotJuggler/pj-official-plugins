# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")
pj_wasm_common_library(utf8_path)

# Only the LZF decompressor: the compressor serves the test fixtures.
enable_language(C)
add_library(pj_wasm_lzf_d STATIC "${CMAKE_CURRENT_LIST_DIR}/contrib/liblzf/lzf_d.c")
target_include_directories(pj_wasm_lzf_d PUBLIC "${CMAKE_CURRENT_LIST_DIR}/contrib/liblzf")
set_target_properties(pj_wasm_lzf_d PROPERTIES POSITION_INDEPENDENT_CODE ON)

pj_wasm_official_plugin(pj_wasm_3d_loader
  SOURCES loader_3d.cpp cloud_common.cpp las_reader.cpp pcd_reader.cpp ply_reader.cpp
  MANIFEST data_load_3d_manifest.hpp=kDataLoad3dManifest
  DATA_SOURCE Load3dSource
  LINKS nlohmann_json::nlohmann_json pj_wasm_lzf_d pj_utf8_path)
