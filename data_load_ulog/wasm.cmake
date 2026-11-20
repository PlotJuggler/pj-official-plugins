# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_ulog_source
  SOURCES ulog_source.cpp ulog_container.cpp
  MANIFEST ulog_manifest.hpp=kUlogManifest
  DATA_SOURCE ULogSource
  LINKS ulog_cpp::ulog_cpp nlohmann_json::nlohmann_json)
