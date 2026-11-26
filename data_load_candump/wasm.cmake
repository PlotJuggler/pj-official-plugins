# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/can_dbc/wasm.cmake")
pj_wasm_common_library(utf8_path)

pj_wasm_official_plugin(pj_wasm_candump_source
  SOURCES candump_source.cpp candump_dialog.cpp candump_parser.cpp csv_dictionary.cpp
  MANIFEST candump_manifest.hpp=kCandumpManifest
  UI candump_dialog.ui=kCandumpDialogUi
  DATA_SOURCE CandumpSource DIALOG CandumpDialog
  LINKS pj_can_dbc pj_utf8_path nlohmann_json::nlohmann_json)
