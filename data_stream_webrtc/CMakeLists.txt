find_package(nlohmann_json REQUIRED)
find_package(LibDataChannel REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# ---------------------------------------------------------------------------
# Plugin shared library.
#
# Source split mirrors the project's core-vs-Qt convention:
#   * video_emit / webrtc_receiver / webrtc_signaling are Qt-FREE and only use
#     libdatachannel + the plugin SDK host views (no Qt6::Widgets).
#   * webrtc_source.cpp holds the StreamSourceBase subclass + the (Qt) dialog
#     plumbing and the plugin export macros.
# ---------------------------------------------------------------------------
add_library(webrtc_source_plugin SHARED
  webrtc_dialog.cpp
  webrtc_source.cpp
  webrtc_receiver.cpp
  webrtc_signaling.cpp
  video_emit.cpp
)
target_compile_features(webrtc_source_plugin PRIVATE cxx_std_20)
target_compile_options(webrtc_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(webrtc_source_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webrtc_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  LibDataChannel::LibDataChannel
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

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(webrtc_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# ---------------------------------------------------------------------------
# Tests (Qt-free). Each test links the Qt-free cores directly and the SDK
# (for serializeVideoFrame / PayloadView). No libdatachannel network use:
# webrtc_receiver_test exercises only the depacketized-AU normalize path via
# the receiver's static helper, not a live PeerConnection.
# ---------------------------------------------------------------------------
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

add_executable(webrtc_receiver_test
  tests/webrtc_receiver_test.cpp
  webrtc_receiver.cpp
  video_emit.cpp
)
target_compile_features(webrtc_receiver_test PRIVATE cxx_std_20)
target_compile_options(webrtc_receiver_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(webrtc_receiver_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webrtc_receiver_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  LibDataChannel::LibDataChannel
  GTest::gtest_main
)
add_test(NAME webrtc_receiver_test COMMAND webrtc_receiver_test)

# ---------------------------------------------------------------------------
# Headless receiver probe (demo/dev tool): the answerer half of the demo/ rig
# without PlotJuggler. Pairs with demo/stream_server.py to confirm the
# camera -> WebRTC -> plugin receive path end-to-end. Not part of the plugin.
# ---------------------------------------------------------------------------
add_executable(webrtc_recv_probe
  demo/recv_probe.cpp
  webrtc_receiver.cpp
  webrtc_signaling.cpp
  video_emit.cpp
)
target_compile_features(webrtc_recv_probe PRIVATE cxx_std_20)
target_compile_options(webrtc_recv_probe PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(webrtc_recv_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(webrtc_recv_probe PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  LibDataChannel::LibDataChannel
)
