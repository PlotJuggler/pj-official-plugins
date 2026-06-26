find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

# Two-layer detection core: anomaly_helpers (Luau-FREE: builtins + JSON report/rule
# helpers) and anomaly_core (adds the engine). The plugin links ONLY anomaly_helpers —
# it submits rules to the host (pj.markers.v1), which executes them, so the plugin
# carries NO script engine. The headless anomaly_runner links anomaly_core (engine).
add_subdirectory(core)

add_library(toolbox_anomaly_detector_plugin SHARED anomaly_detector.cpp)
target_compile_features(toolbox_anomaly_detector_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_anomaly_detector_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_anomaly_detector_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  anomaly_helpers
  pj_toolbox_preview
  nlohmann_json::nlohmann_json
  ${CMAKE_DL_LIBS}
)

pj_embed_manifest(toolbox_anomaly_detector_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/anomaly_detector_manifest.hpp
  VAR_NAME kAnomalyDetectorManifest
)

pj_embed_ui(toolbox_anomaly_detector_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/anomaly_detector_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/anomaly_detector_dialog_ui.hpp
  VAR_NAME kAnomalyDetectorDialogUi
)

pj_emit_plugin_manifest(toolbox_anomaly_detector_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
