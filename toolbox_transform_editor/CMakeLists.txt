find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# The pure text helpers, compiled once and linked into the plugin and both tests.
add_library(toolbox_transform_editor_script_text STATIC script_text.cpp)
set_target_properties(toolbox_transform_editor_script_text PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(toolbox_transform_editor_script_text PUBLIC cxx_std_20)
target_compile_options(toolbox_transform_editor_script_text PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(toolbox_transform_editor_script_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(toolbox_transform_editor_script_text PUBLIC
  pj_derived_recipes plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json)

add_library(toolbox_transform_editor_plugin SHARED transform_editor_plugin.cpp)

target_compile_features(toolbox_transform_editor_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_transform_editor_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_transform_editor_plugin PRIVATE
  toolbox_transform_editor_script_text
  pj_derived_recipes
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
)

target_include_directories(toolbox_transform_editor_plugin PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

pj_embed_ui(toolbox_transform_editor_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/transform_editor_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/transform_editor_dialog_ui.hpp
  VAR_NAME kTransformEditorDialogUi
)

pj_embed_ui(toolbox_transform_editor_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/transform_editor_help.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/transform_editor_help_ui.hpp
  VAR_NAME kHelpDialogUi
)

pj_embed_ui(toolbox_transform_editor_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/function_library.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/function_library_ui.hpp
  VAR_NAME kFunctionLibraryUi
)

pj_embed_ui(toolbox_transform_editor_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/save_function_name.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/save_function_name_ui.hpp
  VAR_NAME kSaveNameUi
)

pj_embed_ui(toolbox_transform_editor_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/overwrite_function.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/overwrite_function_ui.hpp
  VAR_NAME kOverwriteUi
)

pj_embed_ui(toolbox_transform_editor_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/create_recipe.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/create_recipe_ui.hpp
  VAR_NAME kCreateRecipeUi
)

pj_embed_ui(toolbox_transform_editor_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/rename_var.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/rename_var_ui.hpp
  VAR_NAME kRenameVarUi
)

pj_embed_manifest(toolbox_transform_editor_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/transform_editor_manifest.hpp
  VAR_NAME kTransformEditorManifest
)

pj_emit_plugin_manifest(toolbox_transform_editor_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(toolbox_transform_editor_plugin
  REQUIRED_EXPORTS PJ_get_toolbox_vtable PJ_get_dialog_vtable)

# --- Unit tests (also manifest.json suggested_sdk_version / floor_test) ---
# One executable, so the plugin source is compiled into the tests once: the trial run, Create / Modify /
# load of on-demand recipes, the asynchronous preview with cancellation on close, and the real toolbox
# against a floor host that predates the typed requests and catalog snapshot v2.
if(TARGET GTest::gtest_main)
  add_executable(toolbox_transform_editor_test tests/transform_editor_test.cpp)
  target_compile_features(toolbox_transform_editor_test PRIVATE cxx_std_20)
  target_compile_options(toolbox_transform_editor_test PRIVATE ${PJ_WARNING_FLAGS})
  target_include_directories(toolbox_transform_editor_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(toolbox_transform_editor_test PRIVATE
    toolbox_transform_editor_script_text pj_derived_recipes pj_test_support plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main)
  add_dependencies(toolbox_transform_editor_test toolbox_transform_editor_plugin)
  add_test(NAME toolbox_transform_editor_test COMMAND toolbox_transform_editor_test)

  # The pure text helpers (tokenizer, renaming, layout) need no host: their own executable.
  add_executable(toolbox_transform_editor_script_text_test tests/script_text_test.cpp)
  target_compile_features(toolbox_transform_editor_script_text_test PRIVATE cxx_std_20)
  target_compile_options(toolbox_transform_editor_script_text_test PRIVATE ${PJ_WARNING_FLAGS})
  target_include_directories(toolbox_transform_editor_script_text_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(toolbox_transform_editor_script_text_test PRIVATE
    toolbox_transform_editor_script_text pj_derived_recipes plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json GTest::gtest_main)
  add_test(NAME toolbox_transform_editor_script_text_test COMMAND toolbox_transform_editor_script_text_test)
endif()
