# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_quaternion
  SOURCES quaternion_plugin.cpp
  MANIFEST quaternion_manifest.hpp=kQuaternionManifest
  UI quaternion_dialog.ui=kQuaternionDialogUi
  TOOLBOX QuaternionToolbox DIALOG QuaternionDialog
  LINKS nlohmann_json::nlohmann_json)
