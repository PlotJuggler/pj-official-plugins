# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_navmap_parser
  SOURCES navmap_parser.cpp navmap_msg_decoder.cpp navmap_mesh.cpp
  MANIFEST navmap_manifest.hpp=kNavMapManifest
  MESSAGE_PARSER NavMapParser
  LINKS nlohmann_json::nlohmann_json)
# The vendored nanocdr differs from rosx_introspection's copy, which the ROS parser links: same
# namespace, different Decoder layout. Their inline functions would merge into one definition at
# link time and run against the wrong class, so this copy gets a namespace of its own. Its
# constexpr endianness probe reads an inactive union member, which Clang rejects by default.
target_include_directories(pj_wasm_navmap_parser SYSTEM PRIVATE "${CMAKE_CURRENT_LIST_DIR}/contrib")
target_compile_definitions(pj_wasm_navmap_parser PRIVATE nanocdr=pj_wasm_navmap_nanocdr)
target_compile_options(pj_wasm_navmap_parser PRIVATE -Wno-invalid-constexpr)
