# WebAssembly build of this plugin, included by PlotJuggler's browser build (cmake/PjWasmPlugin.cmake).
#
# Over the browser's own WebSocket (common/websocket's pj_websocket_wasm); its messages are decoded
# by the ROS, Protobuf and JSON parsers. A page served over https:// can open only wss://, so a
# robot's plain ws:// foxglove_bridge needs TLS in front of it.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjWasmPlugin.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../common/websocket/wasm.cmake")
pj_wasm_common_library(base64)

pj_wasm_official_plugin(pj_wasm_foxglove_bridge
  SOURCES foxglove_source.cpp
  MANIFEST foxglove_manifest.hpp=kFoxgloveManifest
  UI foxglove_client.ui=kFoxgloveClientUi
  DATA_SOURCE FoxgloveSource DIALOG FoxgloveDialog
  LINKS nlohmann_json::nlohmann_json pj_base64 pj_websocket_wasm)
