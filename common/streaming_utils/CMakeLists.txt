# Single-purpose ixwebsocket helper: websocket_utils.hpp (synchronous
# connect-and-wait for ix::WebSocket), shared by the websocket-based streaming
# plugins. Consumers link ixwebsocket through their own plugin targets.

add_library(pj_streaming_utils INTERFACE)
target_include_directories(pj_streaming_utils INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(pj_streaming_utils INTERFACE cxx_std_20)
