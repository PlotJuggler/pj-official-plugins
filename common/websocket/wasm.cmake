# WebAssembly backend of the shared WebSocket client (cmake/PjWasmPlugin.cmake), on the browser's
# own WebSocket (<emscripten/websocket.h>); the native CMakeLists builds the IXWebSocket one. It is
# compiled once for every consumer: each compiling it would define PJ::WebSocket::Client and its
# EM_JS entries twice in the one module.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PjWasmPlugin.cmake")

add_library(pj_websocket_wasm STATIC "${CMAKE_CURRENT_LIST_DIR}/websocket_client_wasm.cpp")
target_compile_features(pj_websocket_wasm PUBLIC cxx_std_20)
target_include_directories(pj_websocket_wasm PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(pj_websocket_wasm PUBLIC plotjuggler_sdk::plugin_sdk)
# Emscripten links its WebSocket JS library only when asked; the final executable inherits this,
# or emscripten_websocket_* stays undefined.
target_link_options(pj_websocket_wasm INTERFACE "-lwebsocket.js")
pj_wasm_finalize_target(pj_websocket_wasm)
