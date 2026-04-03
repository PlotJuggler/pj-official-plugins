find_package(nlohmann_json REQUIRED)
find_package(zstd REQUIRED)
find_package(ixwebsocket REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(pj_bridge_protocol_lib STATIC pj_bridge_protocol.cpp)
set_target_properties(pj_bridge_protocol_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(pj_bridge_protocol_lib PUBLIC cxx_std_20)
target_compile_options(pj_bridge_protocol_lib PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(pj_bridge_protocol_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pj_bridge_protocol_lib PUBLIC zstd::libzstd_static)

add_library(pj_bridge_source_plugin SHARED pj_bridge_source.cpp)
target_compile_features(pj_bridge_source_plugin PRIVATE cxx_std_20)
target_compile_options(pj_bridge_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_bridge_source_plugin PRIVATE
  pj_base pj_dialog_sdk nlohmann_json::nlohmann_json pj_bridge_protocol_lib ixwebsocket::ixwebsocket
)

pj_embed_ui(pj_bridge_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/websocket_client.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/websocket_client_ui.hpp
  VAR_NAME kWebSocketClientUi
)

pj_embed_manifest(pj_bridge_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/pj_bridge_manifest.hpp
  VAR_NAME kPjBridgeManifest
)

# --- Unit test for PJ Bridge protocol ---
add_executable(pj_bridge_protocol_test tests/pj_bridge_protocol_test.cpp)
target_compile_features(pj_bridge_protocol_test PRIVATE cxx_std_20)
target_compile_options(pj_bridge_protocol_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(pj_bridge_protocol_test PRIVATE pj_bridge_protocol_lib GTest::gtest_main)
add_test(NAME pj_bridge_protocol_test COMMAND pj_bridge_protocol_test)
