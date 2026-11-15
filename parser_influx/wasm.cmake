# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_influx_parser
  SOURCES influx_parser.cpp
  MANIFEST influx_manifest.hpp=kInfluxManifest
  MESSAGE_PARSER InfluxParser)
