find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(toolbox_colormap_plugin SHARED colormap_plugin.cpp)
target_compile_features(toolbox_colormap_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_colormap_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_colormap_plugin PRIVATE
  pj_base
  pj_dialog_sdk
  nlohmann_json::nlohmann_json
)

pj_embed_ui(toolbox_colormap_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/colormap_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/colormap_dialog_ui.hpp
  VAR_NAME kColormapDialogUi
)

pj_embed_manifest(toolbox_colormap_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/colormap_manifest.hpp
  VAR_NAME kColormapManifest
)
