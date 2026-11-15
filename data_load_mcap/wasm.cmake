# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_mcap_source
  SOURCES mcap_source.cpp
  MANIFEST mcap_manifest.hpp=kMcapManifest
  UI dialog_mcap.ui=kDialogMcapUi
  DATA_SOURCE McapSource DIALOG McapDialog
  LINKS nlohmann_json::nlohmann_json LZ4::lz4_static zstd::libzstd_static)
target_include_directories(pj_wasm_mcap_source SYSTEM PRIVATE "${CMAKE_CURRENT_LIST_DIR}/contrib")
