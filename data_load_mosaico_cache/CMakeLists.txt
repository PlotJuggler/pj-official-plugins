find_package(nlohmann_json REQUIRED)
# Arrow (and the vendored mcap headers + lz4/zstd) arrive transitively through
# mosaico_ingest_core; a build without Arrow fails there, at the real consumer.
if(NOT TARGET mosaico_ingest_core)
  message(FATAL_ERROR "data_load_mosaico_cache requires the mosaico_ingest_core target (common/mosaico_ingest_core)")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(mosaico_cache_source_plugin SHARED mosaico_cache_source.cpp)
target_compile_features(mosaico_cache_source_plugin PRIVATE cxx_std_20)
target_compile_options(mosaico_cache_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(mosaico_cache_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk mosaico_ingest_core nlohmann_json::nlohmann_json
)

pj_embed_manifest(mosaico_cache_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/mosaico_cache_manifest.hpp
  VAR_NAME kMosaicoCacheManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(mosaico_cache_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(mosaico_cache_source_plugin
  REQUIRED_EXPORTS PJ_get_data_source_vtable)
