# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

add_library(pj_wasm_csv_parser STATIC
  "${CMAKE_CURRENT_LIST_DIR}/csv_parser.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/timestamp_parsing.cpp")
target_compile_features(pj_wasm_csv_parser PUBLIC cxx_std_20)
target_include_directories(pj_wasm_csv_parser PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(pj_wasm_csv_parser PUBLIC date::date plotjuggler_sdk::base)
pj_wasm_finalize_targets(pj_wasm_csv_parser)

pj_wasm_official_plugin(pj_wasm_csv_source
  SOURCES csv_source.cpp csv_dialog.cpp
  MANIFEST csv_manifest.hpp=kCsvManifest
  UI dataload_csv.ui=kDataLoadCsvUi datetimehelp.ui=kDateTimeHelpUi
  DATA_SOURCE CsvSource DIALOG CsvDialog
  LINKS pj_wasm_csv_parser nlohmann_json::nlohmann_json)
