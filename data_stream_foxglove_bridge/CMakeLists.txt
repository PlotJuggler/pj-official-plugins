find_package(nlohmann_json REQUIRED)
find_package(ixwebsocket REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(foxglove_source_plugin SHARED foxglove_source.cpp)
target_compile_features(foxglove_source_plugin PRIVATE cxx_std_20)
target_compile_options(foxglove_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(foxglove_source_plugin PRIVATE
  pj_base pj_dialog_sdk nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket
)

pj_embed_ui(foxglove_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/foxglove_client.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/foxglove_client_ui.hpp
  VAR_NAME kFoxgloveClientUi
)

pj_embed_manifest(foxglove_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/foxglove_manifest.hpp
  VAR_NAME kFoxgloveManifest
)
