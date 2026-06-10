find_package(nlohmann_json REQUIRED)
find_package(ixwebsocket REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(foxglove_source_plugin SHARED foxglove_source.cpp)
target_compile_features(foxglove_source_plugin PRIVATE cxx_std_20)
target_compile_options(foxglove_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(foxglove_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket pj_base64
  pj_array_policy
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
target_link_libraries(foxglove_protocol_test PRIVATE GTest::gtest_main pj_base64)
add_test(NAME foxglove_protocol_test COMMAND foxglove_protocol_test)
