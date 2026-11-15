# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_json_parser
  SOURCES json_parser.cpp
  MANIFEST json_manifest.hpp=kJsonManifest
  UI json_parser_options.ui=kJsonParserOptionsUi
  MESSAGE_PARSER JsonParser DIALOG JsonParserDialog
  LINKS nlohmann_json::nlohmann_json)
