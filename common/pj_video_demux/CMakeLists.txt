# pj_video_demux — lazy, non-resident container demux + access-unit rewrite
# (H.264/H.265 Annex-B, AV1 OBU temporal units). Used by data_load_mp4 and
# data_load_lerobot to emit per-frame PJ.VideoFrame entries without ever
# buffering the whole video.
#
# Skipped when ffmpeg is unavailable (same pattern as common/arrow_helpers): a real consumer
# does its own find_package(ffmpeg REQUIRED).

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

# Linked into SHARED plugin .so files: must be position-independent.
set_target_properties(pj_video_demux PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(pj_video_demux PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(pj_video_demux PUBLIC
  plotjuggler_sdk::plugin_sdk
  ffmpeg::avformat
  ffmpeg::avcodec
  ffmpeg::avutil
)
# The lazy reader opens the host's UTF-8 path through it.
target_link_libraries(pj_video_demux PRIVATE pj_utf8_path)

# ffmpeg/8.1.2's ConanCenter recipe compiles avformat with the udp/tcp protocol and the
# Windows Schannel TLS backend unconditionally on Windows (`disable_all_protocols=True` only
# affects demuxer selection, not the linker symbol footprint). Consumers must link the system
# libraries those objects reference:
#   - avformat.lib(udp.o)          -> Winsock:       ws2_32
#   - avformat.lib(tls_schannel.o) -> Schannel + CNG: secur32, crypt32, bcrypt, ncrypt
if(WIN32)
  target_link_libraries(pj_video_demux PUBLIC ws2_32 secur32 crypt32 bcrypt ncrypt)
endif()

find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(pj_video_demux_test tests/video_demux_test.cpp)
  target_compile_features(pj_video_demux_test PRIVATE cxx_std_20)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_video_demux_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_compile_definitions(pj_video_demux_test PRIVATE
    PJ_VIDEO_DEMUX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  target_link_libraries(pj_video_demux_test PRIVATE pj_video_demux pj_utf8_path GTest::gtest_main)
  add_test(NAME pj_video_demux_test COMMAND pj_video_demux_test)
endif()
