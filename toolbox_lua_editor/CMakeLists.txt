find_package(lua REQUIRED)
find_package(sol2 REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(pybind11 REQUIRED)
find_package(Python3 COMPONENTS Development REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

add_library(toolbox_lua_editor_plugin SHARED lua_editor_plugin.cpp)
target_compile_features(toolbox_lua_editor_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_lua_editor_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_lua_editor_plugin PRIVATE
  pj_base
  pj_dialog_sdk
  lua::lua
  sol2::sol2
  nlohmann_json::nlohmann_json
  pybind11::embed
)

pj_embed_manifest(toolbox_lua_editor_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/lua_editor_manifest.hpp
  VAR_NAME kLuaEditorManifest
)

pj_embed_ui(toolbox_lua_editor_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/lua_editor_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/lua_editor_dialog_ui.hpp
  VAR_NAME kLuaEditorDialogUi
)
