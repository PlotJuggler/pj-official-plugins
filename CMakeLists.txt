cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# Dual-mode: standalone project OR subdirectory of plotjuggler_sdk.
#
# Detection: if plotjuggler_sdk::plugin_sdk already exists as a target, we are a subdirectory
# and all deps are already provided by the parent project.
# ---------------------------------------------------------------------------

if(TARGET plotjuggler_sdk::plugin_sdk)
  # Subdirectory mode — deps already provided by parent.
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

  # Internal helper libraries shared by several plugins.
  add_subdirectory(common/arrow_helpers)
  add_subdirectory(common/base64)
  add_subdirectory(common/pj_video_demux)
  add_subdirectory(common/array_policy)
  add_subdirectory(common/streaming_utils)
  add_subdirectory(common/laser_scan)
  add_subdirectory(common/pointcloud_color)

  # Enabled entries track the v4 ABI (PJ_ABI_VERSION == 4): service-
  # registry API, Arrow C Data Interface at the write boundary, and the
  # PJ_NOEXCEPT vtable surface. Commented-out entries stopped at the
  # v1/v2 ABI (old bindWriteHost, dialogContext, etc.) and need a fresh
  # v4 port — v3 was never an official release, so there is no v3
  # intermediate stop. Uncomment each as it is brought over to v4.
  add_subdirectory(data_load_csv)
  add_subdirectory(data_load_mcap)
  add_subdirectory(data_load_parquet)
  add_subdirectory(data_load_ulog)
  add_subdirectory(data_load_3d)
  # data_load_lerobot and data_load_mp4 are intentionally NOT in this aggregate
  # list: they depend on FFmpeg (via common/pj_video_demux), which this build
  # does not provide. Both build per-plugin against their own conanfile (CI's
  # arrow-plugin-build / per-plugin-build and the release path).
  add_subdirectory(parser_json)
  add_subdirectory(parser_protobuf)
  add_subdirectory(parser_ros)
  add_subdirectory(parser_data_tamer)
  add_subdirectory(data_stream_zmq)
  add_subdirectory(data_stream_mqtt)
  add_subdirectory(data_stream_udp)
  add_subdirectory(data_stream_foxglove_bridge)
  add_subdirectory(data_stream_webrtc)
  add_subdirectory(data_stream_pj_bridge)
  add_subdirectory(data_stream_dummy)
  add_subdirectory(data_stream_lsl)
  add_subdirectory(toolbox_fft)
  add_subdirectory(toolbox_colormap)
  add_subdirectory(toolbox_quaternion)
  add_subdirectory(toolbox_reactive_scripts_editor)
  add_subdirectory(toolbox_mosaico)
  add_subdirectory(toolbox_transform_editor)
  return()
endif()

# ---------------------------------------------------------------------------
# Standalone build
# ---------------------------------------------------------------------------

project(pj_ported_plugins LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Compiler warnings (matches plotjuggler_sdk)
# ---------------------------------------------------------------------------

if(MSVC)
  set(PJ_WARNING_FLAGS /W4 /WX /permissive-)
else()
  set(PJ_WARNING_FLAGS
    -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-qual -Wconversion -Woverloaded-virtual -Wpedantic
  )
  # GCC's -O3 inliner produces well-known stringop-overflow / array-bounds
  # FALSE positives on std::vector<byte> insert/append (e.g. data_stream_pj_bridge's
  # buildTestFrame on GCC 13). These are vendored upstream plugins; demote those two
  # classes to warnings so a compiler-version quirk can't break the aggregate build.
  # Real overflows in first-party code still warn loudly.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND PJ_WARNING_FLAGS -Wno-error=stringop-overflow -Wno-error=array-bounds)
  endif()
endif()

# ---------------------------------------------------------------------------
# Conan toolchain (convenience fallback — prefer -DCMAKE_TOOLCHAIN_FILE)
# ---------------------------------------------------------------------------

if(EXISTS "${CMAKE_BINARY_DIR}/conan_toolchain.cmake")
  include("${CMAKE_BINARY_DIR}/conan_toolchain.cmake")
endif()

# ---------------------------------------------------------------------------
# Common dependencies (GTest used by most plugins' tests)
# ---------------------------------------------------------------------------

find_package(GTest REQUIRED)

# ---------------------------------------------------------------------------
# PlotJuggler Core SDK (Conan package, shared PlotJuggler JFrog remote).
# The exact version is pinned by Conan (see conanfile.txt / each plugin's
# conanfile.py) — CMakeDeps generates a config for precisely that version, so
# no version is asserted here. Keeping the version out of find_package means a
# core bump only touches the Conan recipe(s), not this file.
# Plugins that still need CPM (parser_ros, parser_data_tamer, data_load_ulog
# for rosx_introspection, data_tamer, ulog_cpp) include cmake/CPM.cmake
# themselves; nothing at the root needs it anymore.
# ---------------------------------------------------------------------------

find_package(plotjuggler_sdk CONFIG REQUIRED
  COMPONENTS base plugin_sdk plugin_host
)

# ---------------------------------------------------------------------------
# Plugin output directory and testing
# ---------------------------------------------------------------------------

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

enable_testing()

# ---------------------------------------------------------------------------
# Internal helper libraries shared by several plugins.
# Added before any plugin subdir so they are visible regardless of whether
# the build picks a single plugin or all of them.
#
# EXCEPTION: common/arrow_helpers is configured in single-plugin
# mode ONLY for its real consumers. Merely configuring it pulls in the SYSTEM
# Arrow CMake config, whose FindProtobufAlt re-includes whatever Protobuf is on
# CMAKE_PREFIX_PATH — colliding with the Conan protobuf (6.x + abseil) that a
# non-Arrow plugin (e.g. parser_protobuf) legitimately requires. The other
# helpers are safe to always configure (base64 header-only; pj_video_demux is
# find_package(ffmpeg QUIET); array_policy needs nlohmann_json, which every
# plugin already has; laser_scan is SDK-only).
# ---------------------------------------------------------------------------

set(PJ_ARROW_HELPER_CONSUMERS data_load_parquet data_load_lerobot)
if(DEFINED PJ_BUILD_PLUGIN)
  if(PJ_BUILD_PLUGIN IN_LIST PJ_ARROW_HELPER_CONSUMERS)
    add_subdirectory(common/arrow_helpers)
  endif()
else()
  add_subdirectory(common/arrow_helpers)
endif()
add_subdirectory(common/base64)
add_subdirectory(common/pj_video_demux)
add_subdirectory(common/array_policy)
add_subdirectory(common/streaming_utils)
add_subdirectory(common/laser_scan)
add_subdirectory(common/pointcloud_color)

# ---------------------------------------------------------------------------
# Plugins — use -DPJ_BUILD_PLUGIN=<dir> to build a single plugin (CI mode),
# or omit to build all plugins (local development).
# Each plugin's CMakeLists.txt calls find_package for its own dependencies.
# ---------------------------------------------------------------------------

if(DEFINED PJ_BUILD_PLUGIN)
  add_subdirectory(${PJ_BUILD_PLUGIN})
else()
  add_subdirectory(parser_json)
  add_subdirectory(parser_protobuf)
  add_subdirectory(parser_ros)
  add_subdirectory(parser_data_tamer)
  add_subdirectory(data_load_csv)
  add_subdirectory(data_load_mcap)
  add_subdirectory(data_load_parquet)
  add_subdirectory(data_load_ulog)
  add_subdirectory(data_load_3d)
  # data_load_lerobot and data_load_mp4 are intentionally NOT in this aggregate
  # list: they depend on FFmpeg (via common/pj_video_demux), which this build
  # does not provide. Both build per-plugin against their own conanfile (CI's
  # arrow-plugin-build / per-plugin-build and the release path).
  add_subdirectory(data_stream_zmq)
  add_subdirectory(data_stream_mqtt)
  add_subdirectory(data_stream_udp)
  add_subdirectory(data_stream_foxglove_bridge)
  add_subdirectory(data_stream_webrtc)
  add_subdirectory(data_stream_pj_bridge)
  add_subdirectory(data_stream_dummy)
  add_subdirectory(data_stream_lsl)
  add_subdirectory(toolbox_fft)
  add_subdirectory(toolbox_colormap)
  add_subdirectory(toolbox_quaternion)
  add_subdirectory(toolbox_mosaico)
endif()
