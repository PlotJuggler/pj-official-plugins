# pj_video_demux — lazy, non-resident container demux + access-unit rewrite
# (H.264/H.265 Annex-B, AV1 OBU temporal units). Used by data_load_mp4 and
# data_load_lerobot to emit per-frame PJ.VideoFrame entries without ever
# buffering the whole video.
#
# Skipped gracefully when ffmpeg is unavailable, mirroring common/arrow_helpers:
# builds that exercise only non-ffmpeg plugins still configure this subdir from
# the top-level CMakeLists.txt but have no ffmpeg Conan dep. A real consumer
# does its own find_package(ffmpeg REQUIRED), so it never silently runs without
# the target.

if(NOT TARGET ffmpeg::avformat)
  find_package(ffmpeg QUIET CONFIG)
  if(NOT TARGET ffmpeg::avformat)
    message(STATUS "pj_video_demux: ffmpeg not found, skipping shared helper library")
    return()
  endif()
endif()

add_library(pj_video_demux STATIC video_demux.cpp)
target_compile_features(pj_video_demux PUBLIC cxx_std_20)
if(DEFINED PJ_WARNING_FLAGS)
  target_compile_options(pj_video_demux PRIVATE ${PJ_WARNING_FLAGS})
endif()

# Static lib goes inside SHARED plugin .so files → must be position-independent.
set_target_properties(pj_video_demux PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(pj_video_demux PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# PJ::Span / PJ::Expected come from plugin_sdk; libav* used only for indexing.
target_link_libraries(pj_video_demux PUBLIC
  plotjuggler_sdk::plugin_sdk
  ffmpeg::avformat
  ffmpeg::avcodec
  ffmpeg::avutil
)

# --- Unit tests: pure byte-rewrite cases + committed h264/hevc/av1 fixtures
#     (+ an env-gated external e2e via PJ_TEST_VIDEO) ---
find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(pj_video_demux_test tests/video_demux_test.cpp)
  target_compile_features(pj_video_demux_test PRIVATE cxx_std_20)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_video_demux_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  # Committed h264/hevc/av1 fixtures drive the per-codec index+read e2e tests.
  target_compile_definitions(pj_video_demux_test PRIVATE
    PJ_VIDEO_DEMUX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  target_link_libraries(pj_video_demux_test PRIVATE pj_video_demux GTest::gtest_main)
  add_test(NAME pj_video_demux_test COMMAND pj_video_demux_test)
endif()
