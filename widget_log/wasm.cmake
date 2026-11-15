# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

# A Widget-family plugin: it links the host's own Qt, so the host's Qt-version and compiler-family
# admission checks hold by construction.
pj_wasm_official_plugin(pj_wasm_log_widget
  SOURCES log_filter.cpp log_loader.cpp log_store.cpp log_viewer_plugin.cpp log_viewer_widget.cpp
          log_table_model.cpp log_viewer.ui
  MANIFEST log_viewer_manifest.hpp=kLogViewerManifest
  WIDGET LogViewerPlugin
  LINKS Qt6::Widgets nlohmann_json::nlohmann_json)
set_target_properties(pj_wasm_log_widget PROPERTIES AUTOMOC ON AUTOUIC ON)
