# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/can_dbc/wasm.cmake")
pj_wasm_common_library(utf8_path)
# The BLF reader at this plugin's pin (the host's lblf_SOURCE_DIR), with this plugin's fixes.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/lblf.cmake")
pj_add_lblf_library(pj_wasm_lblf "${lblf_SOURCE_DIR}")

pj_wasm_official_plugin(pj_wasm_blf_source
  SOURCES blf_source.cpp blf_dialog.cpp blf_frames.cpp
  MANIFEST blf_manifest.hpp=kBlfManifest
  UI blf_dialog.ui=kBlfDialogUi
  DATA_SOURCE BlfSource DIALOG BlfDialog
  LINKS pj_can_dbc pj_utf8_path nlohmann_json::nlohmann_json pj_wasm_lblf)
