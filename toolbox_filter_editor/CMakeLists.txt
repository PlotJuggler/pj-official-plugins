find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

add_library(toolbox_filter_editor_plugin SHARED filter_editor_plugin.cpp)
target_compile_features(toolbox_filter_editor_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_filter_editor_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_filter_editor_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  ${CMAKE_DL_LIBS}
)

pj_embed_manifest(toolbox_filter_editor_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/filter_editor_manifest.hpp
  VAR_NAME kFilterEditorManifest
)

pj_embed_ui(toolbox_filter_editor_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/filter_editor_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/filter_editor_dialog_ui.hpp
  VAR_NAME kFilterEditorDialogUi
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(toolbox_filter_editor_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# Built-in predefined transforms catalogue lives in this plugin (it provides
# them); the SDK contract they implement is in plotjuggler_sdk::plugin_sdk.
# Tests guard the per-transform PJ3-mirror math + the SDK contract (the
# default appendTail loops calculateNextPoint, save/load round-trips).
if(TARGET GTest::gtest_main)
  add_executable(builtin_transforms_test builtin_transforms_test.cpp)
  target_compile_features(builtin_transforms_test PRIVATE cxx_std_20)
  target_compile_options(builtin_transforms_test PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(builtin_transforms_test PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json
    GTest::gtest_main
  )
  add_test(NAME builtin_transforms_test COMMAND builtin_transforms_test)
endif()
