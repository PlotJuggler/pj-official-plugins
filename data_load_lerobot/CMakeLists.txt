find_package(nlohmann_json REQUIRED)
if(NOT TARGET Arrow::arrow_static)
  find_package(Arrow REQUIRED)
endif()
if(NOT TARGET Parquet::parquet_static)
  find_package(Parquet REQUIRED)
endif()
find_package(ffmpeg REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# --- Plugin shared library ---
add_library(lerobot_source_plugin SHARED
  src/lerobot_plugin.cpp
  src/dataset_model.cpp
  src/timeline.cpp
  src/flatten_plan.cpp
  src/video_ingest.cpp
)
target_compile_features(lerobot_source_plugin PRIVATE cxx_std_20)
target_compile_options(lerobot_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(lerobot_source_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lerobot_source_plugin PRIVATE
  pj_base pj_dialog_sdk nlohmann_json::nlohmann_json
  Arrow::arrow_static Parquet::parquet_static
  ffmpeg::avformat ffmpeg::avcodec ffmpeg::avutil
)

pj_embed_ui(lerobot_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/dialog_lerobot.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/dialog_lerobot_ui.hpp
  VAR_NAME kDialogLerobotUi
)

pj_embed_manifest(lerobot_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/lerobot_manifest.hpp
  VAR_NAME kLerobotManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(lerobot_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Unit tests (pure logic: no Arrow / FFmpeg / host needed) ---
function(lerobot_add_test name)
  add_executable(${name} ${ARGN})
  target_compile_features(${name} PRIVATE cxx_std_20)
  target_compile_options(${name} PRIVATE ${PJ_WARNING_FLAGS})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${name} PRIVATE pj_base nlohmann_json::nlohmann_json GTest::gtest_main)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

lerobot_add_test(lerobot_dataset_model_test tests/dataset_model_test.cpp src/dataset_model.cpp)
lerobot_add_test(lerobot_timeline_test     tests/timeline_test.cpp     src/timeline.cpp)
lerobot_add_test(lerobot_flatten_plan_test tests/flatten_plan_test.cpp src/flatten_plan.cpp)

# --- Video ingest test (needs FFmpeg; SKIPs unless LEROBOT_TEST_MP4 is set) ---
add_executable(lerobot_video_ingest_test tests/video_ingest_test.cpp src/video_ingest.cpp)
target_compile_features(lerobot_video_ingest_test PRIVATE cxx_std_20)
target_compile_options(lerobot_video_ingest_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(lerobot_video_ingest_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lerobot_video_ingest_test PRIVATE
  pj_base ffmpeg::avformat ffmpeg::avcodec ffmpeg::avutil GTest::gtest_main
)
add_test(NAME lerobot_video_ingest_test COMMAND lerobot_video_ingest_test)
