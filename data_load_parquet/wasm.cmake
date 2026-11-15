# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_parquet_source
  SOURCES parquet_source.cpp ../common/arrow_helpers/arrow_helpers.cpp
          # date::locate_zone needs date's tz library, which the browser build leaves out. Built
          # here against the (absent) OS tz database, no remote download.
          "${date_SOURCE_DIR}/src/tz.cpp"
  INCLUDES ../common/arrow_helpers/include
  MANIFEST parquet_manifest.hpp=kParquetManifest
  UI dataload_parquet.ui=kDataLoadParquetUi
  DATA_SOURCE ParquetSource DIALOG ParquetDialog
  LINKS nlohmann_json::nlohmann_json date::date)
target_compile_definitions(pj_wasm_parquet_source PRIVATE USE_OS_TZDB=1 HAS_REMOTE_API=0)
target_link_libraries(pj_wasm_parquet_source PUBLIC Arrow::arrow_static Parquet::parquet_static)
