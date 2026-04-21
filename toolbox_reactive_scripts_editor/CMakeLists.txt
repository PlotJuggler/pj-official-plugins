find_package(lua REQUIRED)
find_package(sol2 REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(pybind11 REQUIRED)
find_package(Python3 COMPONENTS Development REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

add_library(toolbox_reactive_scripts_editor_plugin SHARED reactive_script_editor.cpp)
target_compile_features(toolbox_reactive_scripts_editor_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_reactive_scripts_editor_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_reactive_scripts_editor_plugin PRIVATE
  pj_base
  pj_dialog_sdk
  lua::lua
  sol2::sol2
  nlohmann_json::nlohmann_json
  pybind11::embed
  ${CMAKE_DL_LIBS}
)

# Bundle the Python stdlib next to the plugin binary so release_tools can include
# it in the distribution zip. Python3_STDLIB is set by find_package(Python3) and
# points to the lib/python3.x directory of the Conan-managed CPython installation.
if(Python3_STDLIB)
  add_custom_command(TARGET toolbox_reactive_scripts_editor_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${Python3_STDLIB}"
      "$<TARGET_FILE_DIR:toolbox_reactive_scripts_editor_plugin>/python3.12"
    COMMENT "Bundling Python stdlib alongside plugin binary"
  )
endif()

pj_embed_manifest(toolbox_reactive_scripts_editor_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/reactive_script_editor_manifest.hpp
  VAR_NAME kReactiveScriptEditorManifest
)

pj_embed_ui(toolbox_reactive_scripts_editor_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/reactive_script_editor_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/reactive_script_editor_dialog_ui.hpp
  VAR_NAME kReactiveScriptEditorDialogUi
)
