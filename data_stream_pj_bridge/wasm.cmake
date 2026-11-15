# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
#
# It connects through the browser's own WebSocket (pj_bridge_websocket_wasm.cpp on
# <emscripten/websocket.h>); the native IXWebSocket backend is never compiled here, so no socket
# library enters the module. Data frames are zstd-compressed.
#
# Symbol audit (PlotJuggler's root CLAUDE.md), emsdk llvm-nm -A --defined-only --extern-only on
# libpj_wasm_pj_bridge.a against every archive and object of the browser app's link plus the
# multithread sysroot archives: its strong definitions (the two static getters,
# PJ::BridgeProtocol::*, the pj_bridge_websocket EM_JS entries) exist nowhere else, and the few
# hundred symbols it shares are weak instantiations of the libc++, nlohmann_json and SDK headers
# every plugin compiles alike; a shared weak symbol from a plugin's own inline variable or function
# would be a silent ODR merge, so those must stay uniquely named. Zstd stays one copy (libzstd.a),
# and no IXWebSocket symbol is referenced. Repeat it when this changes.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_pj_bridge
  SOURCES pj_bridge_source.cpp pj_bridge_protocol.cpp pj_bridge_websocket_wasm.cpp
  MANIFEST pj_bridge_manifest.hpp=kPjBridgeManifest
  UI websocket_client.ui=kWebSocketClientUi
  DATA_SOURCE PjBridgeSource DIALOG PjBridgeDialog
  LINKS nlohmann_json::nlohmann_json zstd::libzstd_static)
# The same Zstd include workaround as data_load_mcap/wasm.cmake.
target_include_directories(pj_wasm_pj_bridge SYSTEM PRIVATE "${zstd_SOURCE_DIR}/lib")
# Emscripten links its WebSocket JS library only when asked; the final executable inherits this,
# or emscripten_websocket_* stays undefined.
target_link_options(pj_wasm_pj_bridge INTERFACE "-lwebsocket.js")
