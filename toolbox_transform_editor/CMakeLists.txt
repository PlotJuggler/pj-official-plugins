find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(toolbox_transform_editor_plugin SHARED transform_editor_plugin.cpp)

target_compile_features(toolbox_transform_editor_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_transform_editor_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_transform_editor_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
)

# When built as part of the PJ4 app (not standalone), link pj_dialog_engine_qt
# to get access to ChartPreviewWidget for the live preview chart.
if(TARGET pj_dialog_engine_qt)
  target_link_libraries(toolbox_transform_editor_plugin PRIVATE pj_dialog_engine_qt)
  target_compile_definitions(toolbox_transform_editor_plugin PRIVATE PJ_HAS_CHART_PREVIEW=1)
endif()

target_include_directories(toolbox_transform_editor_plugin PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

pj_embed_ui(toolbox_transform_editor_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/transform_editor_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/transform_editor_dialog_ui.hpp
  VAR_NAME kTransformEditorDialogUi
)

pj_embed_manifest(toolbox_transform_editor_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/transform_editor_manifest.hpp
  VAR_NAME kTransformEditorManifest
)

pj_emit_plugin_manifest(toolbox_transform_editor_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
