find_package(nlohmann_json REQUIRED)
find_package(LibDataChannel REQUIRED)
find_package(ixwebsocket REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(webrtc_source_plugin SHARED
  webrtc_dialog.cpp
  webrtc_source.cpp
  video_emit.cpp
  whep_client.cpp
  whep_connection.cpp
)
target_compile_features(webrtc_source_plugin PRIVATE cxx_std_20)
target_compile_options(webrtc_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(webrtc_source_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webrtc_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  LibDataChannel::LibDataChannel
  ixwebsocket::ixwebsocket
)

pj_embed_ui(webrtc_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/datastream_webrtc.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/datastream_webrtc_ui.hpp
  VAR_NAME kDataStreamWebrtcUi
)

pj_embed_manifest(webrtc_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/webrtc_manifest.hpp
  VAR_NAME kWebrtcManifest
)

# Sidecar .pjmanifest.json (manifest.json + abi_major + family) the host scans before dlopen.
pj_emit_plugin_manifest(webrtc_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# Tests link the cores directly plus the SDK (serializeVideoFrame / PayloadView); none uses the libdatachannel network.
add_executable(webrtc_video_emit_test
  tests/video_emit_test.cpp
  video_emit.cpp
)
target_compile_features(webrtc_video_emit_test PRIVATE cxx_std_20)
target_compile_options(webrtc_video_emit_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(webrtc_video_emit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webrtc_video_emit_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  GTest::gtest_main
)
add_test(NAME webrtc_video_emit_test COMMAND webrtc_video_emit_test)

add_executable(whep_client_test
  tests/whep_client_test.cpp
  whep_client.cpp
)
target_compile_features(whep_client_test PRIVATE cxx_std_20)
target_compile_options(whep_client_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(whep_client_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(whep_client_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  ixwebsocket::ixwebsocket
  GTest::gtest
)
add_test(NAME whep_client_test COMMAND whep_client_test)

add_executable(whep_connection_test
  tests/whep_connection_test.cpp
  whep_connection.cpp
  whep_client.cpp
  video_emit.cpp
)
target_compile_features(whep_connection_test PRIVATE cxx_std_20)
target_compile_options(whep_connection_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(whep_connection_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(whep_connection_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  LibDataChannel::LibDataChannel
  ixwebsocket::ixwebsocket
  GTest::gtest_main
)
add_test(NAME whep_connection_test COMMAND whep_connection_test)

add_executable(webrtc_dialog_test
  tests/webrtc_dialog_test.cpp
  webrtc_dialog.cpp
  whep_client.cpp
)
target_compile_features(webrtc_dialog_test PRIVATE cxx_std_20)
target_compile_options(webrtc_dialog_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(webrtc_dialog_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(webrtc_dialog_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  ixwebsocket::ixwebsocket
  GTest::gtest_main
)
add_test(NAME webrtc_dialog_test COMMAND webrtc_dialog_test)

# Dev tool, not part of the plugin: a standalone WHEP client that confirms the receive path against the mediamtx rig or
# any WHEP endpoint.
add_executable(webrtc_recv_probe
  demo/recv_probe.cpp
  whep_connection.cpp
  whep_client.cpp
  video_emit.cpp
)
target_compile_features(webrtc_recv_probe PRIVATE cxx_std_20)
target_compile_options(webrtc_recv_probe PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(webrtc_recv_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webrtc_recv_probe PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  LibDataChannel::LibDataChannel
  ixwebsocket::ixwebsocket
)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(webrtc_source_plugin
  REQUIRED_EXPORTS PJ_get_data_source_vtable PJ_get_dialog_vtable)
