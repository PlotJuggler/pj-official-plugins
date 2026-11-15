# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_arrow_parser
  SOURCES src/mosaico_objects.cpp src/ipc_decoder.cpp src/table_shaper.cpp src/arrow_parser.cpp
  INCLUDES src
  MANIFEST arrow_manifest.hpp=kArrowManifest
  MESSAGE_PARSER ArrowParser
  LINKS nanoarrow::nanoarrow nlohmann_json::nlohmann_json)
target_link_libraries(pj_wasm_arrow_parser PUBLIC Arrow::arrow_static)
