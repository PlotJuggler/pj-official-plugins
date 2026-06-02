find_package(lua REQUIRED)
find_package(sol2 REQUIRED)
find_package(nlohmann_json REQUIRED)

# Python embedding is gated behind an opt-in flag. Default OFF so Linux
# marketplace artifacts ship without a NEEDED dependency on a libpython that
# may not exist on the user's machine — the plugin loads, Lua works, and the
# Python tab is rendered as disabled in the dialog.
# Set PJ_REACTIVE_ENABLE_PYTHON=ON to compile in the embedded interpreter
# (developer/local builds, or release builds on platforms where the libpython
# bundling is known-good).
option(PJ_REACTIVE_ENABLE_PYTHON "Embed CPython via pybind11 (libpython linked)" OFF)

if(PJ_REACTIVE_ENABLE_PYTHON)
  find_package(pybind11 REQUIRED)
  find_package(Python3 COMPONENTS Development REQUIRED)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

add_library(toolbox_reactive_scripts_editor_plugin SHARED reactive_script_editor.cpp)
target_compile_features(toolbox_reactive_scripts_editor_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_reactive_scripts_editor_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_reactive_scripts_editor_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  lua::lua
  sol2::sol2
  nlohmann_json::nlohmann_json
  ${CMAKE_DL_LIBS}
)

if(PJ_REACTIVE_ENABLE_PYTHON)
  target_link_libraries(toolbox_reactive_scripts_editor_plugin PRIVATE pybind11::embed)
  target_compile_definitions(toolbox_reactive_scripts_editor_plugin PRIVATE PJ_REACTIVE_HAS_PYTHON)

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

  # When the Conan cpython package is built shared (required on Windows/MSVC to
  # support .pyd extensions), python3XX.dll must sit next to the plugin binary so
  # Windows' alternate search order (used when the plugin is loaded by absolute
  # path) resolves it. The guard on TYPE makes this a no-op with static cpython
  # or on Linux.
  if(WIN32 AND TARGET Python3::Python)
    get_target_property(_pj_python_type Python3::Python TYPE)
    if(_pj_python_type STREQUAL "SHARED_LIBRARY")
      add_custom_command(TARGET toolbox_reactive_scripts_editor_plugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:Python3::Python>
          "$<TARGET_FILE_DIR:toolbox_reactive_scripts_editor_plugin>"
        COMMENT "Bundling python3XX.dll alongside plugin binary"
      )
    endif()
  endif()
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

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(toolbox_reactive_scripts_editor_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
