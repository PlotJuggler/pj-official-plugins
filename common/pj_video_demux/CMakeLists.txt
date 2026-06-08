# pj_video_demux — lazy, non-resident container demux + AVCC→Annex-B helper.
# Used by data_load_mp4 (and future file-backed VideoFrame loaders) to emit
# per-frame PJ.VideoFrame entries without ever buffering the whole video.
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

# --- Unit test (fixture-free for the AVCC→Annex-B core; env-gated e2e) ---
find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(pj_video_demux_test tests/video_demux_test.cpp)
  target_compile_features(pj_video_demux_test PRIVATE cxx_std_20)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_video_demux_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(pj_video_demux_test PRIVATE pj_video_demux GTest::gtest_main)
  add_test(NAME pj_video_demux_test COMMAND pj_video_demux_test)
endif()
