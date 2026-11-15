# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_csv_source
  SOURCES csv_source.cpp csv_dialog.cpp csv_parser.cpp timestamp_parsing.cpp
  MANIFEST csv_manifest.hpp=kCsvManifest
  UI dataload_csv.ui=kDataLoadCsvUi datetimehelp.ui=kDateTimeHelpUi
  DATA_SOURCE CsvSource DIALOG CsvDialog
  LINKS nlohmann_json::nlohmann_json date::date)
