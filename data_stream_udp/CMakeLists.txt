find_package(asio REQUIRED)
find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(udp_source_plugin SHARED udp_source.cpp)
target_compile_features(udp_source_plugin PRIVATE cxx_std_20)
target_compile_options(udp_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(udp_source_plugin PRIVATE plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json asio::asio)

pj_embed_ui(udp_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/datastream_udp.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/datastream_udp_ui.hpp
  VAR_NAME kDataStreamUdpUi
)

pj_embed_manifest(udp_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/udp_manifest.hpp
  VAR_NAME kUdpManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(udp_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
