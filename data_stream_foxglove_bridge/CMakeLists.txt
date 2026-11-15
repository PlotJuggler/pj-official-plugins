find_package(nlohmann_json REQUIRED)
find_package(ixwebsocket REQUIRED)

# The WebSocket transport is common/websocket: its IXWebSocket backend here; a WebAssembly build
# compiles its browser backend instead and needs no IXWebSocket.

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(foxglove_source_plugin SHARED foxglove_source.cpp)
target_compile_features(foxglove_source_plugin PRIVATE cxx_std_20)
target_compile_options(foxglove_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(foxglove_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json pj_base64 pj_websocket_native ixwebsocket::ixwebsocket
)

pj_embed_ui(foxglove_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/foxglove_client.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/foxglove_client_ui.hpp
  VAR_NAME kFoxgloveClientUi
)

pj_embed_manifest(foxglove_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/foxglove_manifest.hpp
  VAR_NAME kFoxgloveManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(foxglove_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Unit test for Foxglove protocol (header-only) ---
add_executable(foxglove_protocol_test tests/foxglove_protocol_test.cpp)
target_compile_features(foxglove_protocol_test PRIVATE cxx_std_20)
target_compile_options(foxglove_protocol_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(foxglove_protocol_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(foxglove_protocol_test PRIVATE GTest::gtest_main pj_base64 nlohmann_json::nlohmann_json)
add_test(NAME foxglove_protocol_test COMMAND foxglove_protocol_test)

# --- Unit test for the demand-subscription control plane (header-only, no sockets) ---
add_executable(foxglove_topic_subscription_test tests/foxglove_topic_subscription_test.cpp)
target_compile_features(foxglove_topic_subscription_test PRIVATE cxx_std_20)
target_compile_options(foxglove_topic_subscription_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(foxglove_topic_subscription_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(foxglove_topic_subscription_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  GTest::gtest_main
)
add_test(NAME foxglove_topic_subscription_test COMMAND foxglove_topic_subscription_test)

# --- Connection lifecycle: the source and dialog against a fake transport (no sockets) ---
add_executable(foxglove_connection_test tests/foxglove_connection_test.cpp)
target_compile_features(foxglove_connection_test PRIVATE cxx_std_20)
target_compile_options(foxglove_connection_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(foxglove_connection_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(foxglove_connection_test PRIVATE
  plotjuggler_sdk::plugin_sdk plotjuggler_sdk::plugin_host nlohmann_json::nlohmann_json pj_base64 pj_websocket_fake
  GTest::gtest_main
)
add_test(NAME foxglove_connection_test COMMAND foxglove_connection_test)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(foxglove_source_plugin
  REQUIRED_EXPORTS PJ_get_data_source_vtable PJ_get_dialog_vtable)
