# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
#
# It connects through the browser's own WebSocket (common/websocket's pj_websocket_wasm); the
# native IXWebSocket backend is never compiled here. Data frames are zstd-compressed.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/websocket/wasm.cmake")

pj_wasm_official_plugin(pj_wasm_pj_bridge
  SOURCES pj_bridge_source.cpp pj_bridge_protocol.cpp
  MANIFEST pj_bridge_manifest.hpp=kPjBridgeManifest
  UI websocket_client.ui=kWebSocketClientUi
  DATA_SOURCE PjBridgeSource DIALOG PjBridgeDialog
  LINKS nlohmann_json::nlohmann_json zstd::libzstd_static pj_websocket_wasm)
