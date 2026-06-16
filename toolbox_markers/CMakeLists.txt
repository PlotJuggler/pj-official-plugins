find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

add_library(toolbox_markers_plugin SHARED markers_plugin.cpp)
target_compile_features(toolbox_markers_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_markers_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_markers_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
)

pj_embed_manifest(toolbox_markers_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/markers_manifest.hpp
  VAR_NAME kMarkersManifest
)

pj_embed_ui(toolbox_markers_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/markers_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/markers_dialog_ui.hpp
  VAR_NAME kMarkersDialogUi
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host scans pre-dlopen.
pj_emit_plugin_manifest(toolbox_markers_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
