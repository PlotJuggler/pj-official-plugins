# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")
pj_wasm_common_library(utf8_path)

pj_wasm_official_plugin(pj_wasm_data_exporter
  SOURCES arrow_series_reader.cpp data_exporter.cpp export_core.cpp plugin_entry.cpp
  MANIFEST data_exporter_manifest.hpp=kDataExporterManifest
  UI exporter_dialog.ui=kDataExporterDialogUi
  TOOLBOX DataExporterToolbox DIALOG DataExporterDialog
  LINKS nlohmann_json::nlohmann_json pj_utf8_path)
target_link_libraries(pj_wasm_data_exporter PUBLIC Arrow::arrow_static Parquet::parquet_static)
