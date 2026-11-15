# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
#
# It connects through the browser's own WebSocket (pj_bridge_websocket_wasm.cpp on
# <emscripten/websocket.h>); the native IXWebSocket backend is never compiled here, so no socket
# library enters the module. Data frames are zstd-compressed.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")

pj_wasm_official_plugin(pj_wasm_pj_bridge
  SOURCES pj_bridge_source.cpp pj_bridge_protocol.cpp pj_bridge_websocket_wasm.cpp
  MANIFEST pj_bridge_manifest.hpp=kPjBridgeManifest
  UI websocket_client.ui=kWebSocketClientUi
  DATA_SOURCE PjBridgeSource DIALOG PjBridgeDialog
  LINKS nlohmann_json::nlohmann_json zstd::libzstd_static)
# Emscripten links its WebSocket JS library only when asked; the final executable inherits this,
# or emscripten_websocket_* stays undefined.
target_link_options(pj_wasm_pj_bridge INTERFACE "-lwebsocket.js")
