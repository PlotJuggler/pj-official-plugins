find_package(nlohmann_json REQUIRED)
find_package(ffmpeg REQUIRED)
find_package(date REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# --- Plugin shared library ---
add_library(mp4_source_plugin SHARED mp4_source.cpp)
target_compile_features(mp4_source_plugin PRIVATE cxx_std_20)
target_compile_options(mp4_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(mp4_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json date::date
  ffmpeg::avformat ffmpeg::avutil
  pj_video_demux
)

pj_embed_manifest(mp4_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/mp4_manifest.hpp
  VAR_NAME kMp4Manifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(mp4_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Unit test for the ISO-8601 → epoch-ns helper (pure, no FFmpeg) ---
add_executable(mp4_iso8601_test tests/mp4_iso8601_test.cpp)
target_compile_features(mp4_iso8601_test PRIVATE cxx_std_20)
target_compile_options(mp4_iso8601_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(mp4_iso8601_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mp4_iso8601_test PRIVATE
  date::date GTest::gtest_main
)
add_test(NAME mp4_iso8601_test COMMAND mp4_iso8601_test)
