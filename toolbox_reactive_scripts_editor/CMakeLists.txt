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
# it in the distribution zip. Layout:
#   <plugin_dir>/python3.12/Lib   — pure-Python modules (Python3_STDLIB)
#   <plugin_dir>/python3.12/DLLs  — native extensions on Windows (Python3_STDARCH)
# On Linux, Python3_STDARCH is nested inside Python3_STDLIB, so a single copy
# covers both pure and native modules.
if(Python3_STDLIB)
  add_custom_command(TARGET toolbox_reactive_scripts_editor_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${Python3_STDLIB}"
      "$<TARGET_FILE_DIR:toolbox_reactive_scripts_editor_plugin>/python3.12/Lib"
    COMMENT "Bundling Python pure stdlib"
  )
endif()

if(WIN32 AND Python3_STDARCH AND NOT Python3_STDARCH STREQUAL Python3_STDLIB)
  add_custom_command(TARGET toolbox_reactive_scripts_editor_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${Python3_STDARCH}"
      "$<TARGET_FILE_DIR:toolbox_reactive_scripts_editor_plugin>/python3.12/DLLs"
    COMMENT "Bundling Python platform-specific extensions (.pyd)"
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
