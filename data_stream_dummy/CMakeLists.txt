include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(dummy_stream_plugin SHARED dummy_stream.cpp)
target_compile_features(dummy_stream_plugin PRIVATE cxx_std_20)
target_compile_options(dummy_stream_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(dummy_stream_plugin PRIVATE plotjuggler_sdk::plugin_sdk)

pj_embed_manifest(dummy_stream_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/dummy_manifest.hpp
  VAR_NAME kDummyManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(dummy_stream_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
