# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")
pj_wasm_common_library(toolbox_preview)

# The toolbox carries no script engine: it submits its rule through the host's
# pj.data_processors.v1 service, so only the Luau-free helper layer is built.
pj_wasm_official_plugin(pj_wasm_anomaly_detector
  SOURCES anomaly_detector.cpp core/anomaly_helpers.cpp
  INCLUDES core
  MANIFEST anomaly_detector_manifest.hpp=kAnomalyDetectorManifest
  UI anomaly_detector_dialog.ui=kAnomalyDetectorDialogUi
  TOOLBOX AnomalyDetectorToolbox DIALOG AnomalyDetectorDialog
  LINKS nlohmann_json::nlohmann_json pj_toolbox_preview)
