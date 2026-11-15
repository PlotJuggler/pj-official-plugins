# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_dummy_stream
  SOURCES dummy_stream.cpp
  MANIFEST dummy_manifest.hpp=kDummyManifest
  DATA_SOURCE DummyStreamer)
