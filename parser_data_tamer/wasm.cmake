# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_data_tamer
  SOURCES data_tamer_parser.cpp
  MANIFEST data_tamer_manifest.hpp=kDataTamerManifest
  MESSAGE_PARSER DataTamerParserPlugin
  LINKS data_tamer_parser)
# data_tamer_parser.hpp defines TypeField::operator== out of class without `inline`, so parser_ros
# and this plugin, each including it, would emit two strong definitions into one static binary.
# Renaming the header's namespace in this plugin's translation unit keeps the copies distinct.
target_compile_definitions(pj_wasm_data_tamer PRIVATE DataTamerParser=PjWasmDataTamerPluginParser)
