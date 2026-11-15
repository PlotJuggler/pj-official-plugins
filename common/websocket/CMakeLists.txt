# WebSocket client transport shared by the websocket streaming plugins
# (data_stream_pj_bridge, data_stream_foxglove_bridge): <pj_websocket/websocket_client.hpp>.
#
# The backend is chosen by source file, compiled into each consumer with its own flags:
#   pj_websocket_native  websocket_client_native.cpp (IXWebSocket). Consumers link
#                        ixwebsocket::ixwebsocket themselves, as with common/streaming_utils.
#   pj_websocket_fake    testing/fake_websocket_client.cpp, for the consumers' connection tests.
# A WebAssembly build (PlotJuggler's cmake/PjWasm*.cmake) compiles websocket_client_wasm.cpp
# instead, ONCE for every consumer, and needs no IXWebSocket.

add_library(pj_websocket INTERFACE)
target_include_directories(pj_websocket INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pj_websocket INTERFACE cxx_std_20)

add_library(pj_websocket_native INTERFACE)
target_sources(pj_websocket_native INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/websocket_client_native.cpp)
target_link_libraries(pj_websocket_native INTERFACE pj_websocket)

add_library(pj_websocket_fake INTERFACE)
target_sources(pj_websocket_fake INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/testing/fake_websocket_client.cpp)
target_link_libraries(pj_websocket_fake INTERFACE pj_websocket)

find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(pj_websocket_event_queue_test tests/websocket_event_queue_test.cpp)
  target_compile_options(pj_websocket_event_queue_test PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(pj_websocket_event_queue_test PRIVATE pj_websocket plotjuggler_sdk::plugin_sdk GTest::gtest_main)
  add_test(NAME pj_websocket_event_queue_test COMMAND pj_websocket_event_queue_test)

  # The IXWebSocket backend against loopback servers; only where a consumer provides IXWebSocket.
  find_package(ixwebsocket QUIET)
  if(TARGET ixwebsocket::ixwebsocket)
    add_executable(pj_websocket_native_test tests/websocket_client_native_test.cpp)
    target_compile_options(pj_websocket_native_test PRIVATE ${PJ_WARNING_FLAGS})
    target_link_libraries(pj_websocket_native_test PRIVATE
      pj_websocket_native plotjuggler_sdk::plugin_sdk ixwebsocket::ixwebsocket GTest::gtest_main)
    add_test(NAME pj_websocket_native_test COMMAND pj_websocket_native_test)
  endif()
endif()
