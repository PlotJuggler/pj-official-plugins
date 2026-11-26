# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/can_dbc/wasm.cmake")
pj_wasm_common_library(utf8_path)
# The MDF reader at this plugin's pin (the host's mdflib_SOURCE_DIR), with this plugin's fixes.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/mdflib.cmake")
pj_add_mdflib_library(pj_wasm_mdflib "${mdflib_SOURCE_DIR}")

pj_wasm_official_plugin(pj_wasm_mf4_source
  SOURCES mf4_source.cpp mf4_dialog.cpp mf4_reader.cpp
  MANIFEST mf4_manifest.hpp=kMf4Manifest
  UI mf4_dialog.ui=kMf4DialogUi
  DATA_SOURCE Mf4Source DIALOG Mf4Dialog
  LINKS pj_can_dbc pj_utf8_path nlohmann_json::nlohmann_json pj_wasm_mdflib)
