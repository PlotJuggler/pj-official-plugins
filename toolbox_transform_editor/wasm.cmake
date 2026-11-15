# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

# One archive for what the native build splits into the plugin, its script_text helper library
# and common/derived_recipes (its only consumer here).
pj_wasm_official_plugin(pj_wasm_transform_editor
  SOURCES transform_editor_plugin.cpp script_text.cpp ../common/derived_recipes/recipes.cpp
  INCLUDES ../common/derived_recipes/include
  MANIFEST transform_editor_manifest.hpp=kTransformEditorManifest
  UI transform_editor_dialog.ui=kTransformEditorDialogUi transform_editor_help.ui=kHelpDialogUi
     function_library.ui=kFunctionLibraryUi save_function_name.ui=kSaveNameUi
     overwrite_function.ui=kOverwriteUi create_recipe.ui=kCreateRecipeUi rename_var.ui=kRenameVarUi
  TOOLBOX TransformEditorToolbox DIALOG TransformEditorDialog
  LINKS nlohmann_json::nlohmann_json)
