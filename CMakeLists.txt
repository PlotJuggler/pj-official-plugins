cmake_minimum_required(VERSION 3.22)

# The headless anomaly_runner CLI (and its Luau engine layer: anomaly_core +
# pj_scripting_core) is OFF by default. The cross-platform plugin CI builds only the
# host-driven, Luau-FREE GUI plugins (the GUI Anomaly Detector submits rules to the host,
# which runs Lua), so it does NOT need the private pj_scripting_core package. Build the
# runner with `./build.sh tools/anomaly_runner` (its recipe provides pj_scripting_core)
# or pass -DPJ_BUILD_ANOMALY_RUNNER=ON.
option(PJ_BUILD_ANOMALY_RUNNER "Build the headless anomaly_runner CLI (needs pj_scripting_core)" OFF)

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
  add_subdirectory(common/streaming_utils)
  add_subdirectory(common/laser_scan)
  add_subdirectory(common/grid_map)
  add_subdirectory(common/pointcloud_color)
  add_subdirectory(common/query)
  add_subdirectory(common/toolbox_preview)
  # This block always adds every plugin (no PJ_BUILD_PLUGIN filtering), so its
  # consumers data_load_mf4/data_load_blf are always present — add their shared
  # can_dbc unconditionally too. (The standalone block below, which DOES honor
  # PJ_BUILD_PLUGIN, gates it to avoid the CPM download for unrelated plugins.)
  add_subdirectory(common/can_dbc)

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
  add_subdirectory(data_load_mf4)
  add_subdirectory(data_load_blf)
  add_subdirectory(data_load_3d)
  # data_load_lerobot and data_load_mp4 are intentionally NOT in this aggregate
  # list: they depend on FFmpeg (via common/pj_video_demux), which this build
  # does not provide. Both build per-plugin against their own conanfile (CI's
  # arrow-plugin-build / per-plugin-build and the release path).
  add_subdirectory(parser_json)
  add_subdirectory(parser_protobuf)
  add_subdirectory(parser_ros)
  add_subdirectory(parser_data_tamer)
  add_subdirectory(parser_arrow)
  add_subdirectory(data_stream_zmq)
  add_subdirectory(data_stream_mqtt)
  add_subdirectory(data_stream_udp)
  add_subdirectory(data_stream_foxglove_bridge)
  add_subdirectory(data_stream_webrtc)
  add_subdirectory(data_stream_pj_bridge)
  add_subdirectory(data_stream_dummy)
  # data_stream_lsl is intentionally NOT in this aggregate list: liblsl requires
  # boost>=1.85, which conflicts with the boost/1.81.0 that Arrow's Flight thrift
  # pins here. It builds per-plugin against its own conanfile (like
  # data_load_lerobot/data_load_mp4).
  add_subdirectory(toolbox_fft)
  add_subdirectory(toolbox_data_exporter)
  add_subdirectory(toolbox_quaternion)
  add_subdirectory(toolbox_anomaly_detector)
  if(PJ_BUILD_ANOMALY_RUNNER)
    add_subdirectory(tools/anomaly_runner)
  endif()
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

# enable_testing() does NOT define BUILD_TESTING -- only include(CTest) does, and
# this project never included it. toolbox_mosaico wraps its entire test tree in
# `if(BUILD_TESTING)`, so all 27 of its tests silently never built and `ctest`
# reported only the handful from common/. Declared here rather than via
# include(CTest) to avoid pulling in CTest's dashboard machinery.
option(BUILD_TESTING "Build the testing tree" ON)

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
# find_package(ffmpeg QUIET); laser_scan and grid_map are SDK-only).
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
add_subdirectory(common/streaming_utils)
add_subdirectory(common/laser_scan)
add_subdirectory(common/grid_map)
add_subdirectory(common/pointcloud_color)
add_subdirectory(common/query)
# common/can_dbc has a CPM download side-effect, so configure it only for its
# consumers (skipped when building an unrelated single plugin).
set(PJ_CAN_DBC_CONSUMERS data_load_mf4 data_load_blf)
if(NOT DEFINED PJ_BUILD_PLUGIN OR PJ_BUILD_PLUGIN IN_LIST PJ_CAN_DBC_CONSUMERS)
  add_subdirectory(common/can_dbc)
endif()

# Shared toolbox-preview engine: only the generator toolboxes consume it, so in
# single-plugin (CI) mode add it only when building one of those plugins.
set(PJ_TOOLBOX_PREVIEW_CONSUMERS toolbox_anomaly_detector)
if(DEFINED PJ_BUILD_PLUGIN)
  if(PJ_BUILD_PLUGIN IN_LIST PJ_TOOLBOX_PREVIEW_CONSUMERS)
    add_subdirectory(common/toolbox_preview)
  endif()
else()
  add_subdirectory(common/toolbox_preview)
endif()

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
  add_subdirectory(parser_arrow)
  add_subdirectory(data_load_csv)
  add_subdirectory(data_load_mcap)
  add_subdirectory(data_load_parquet)
  add_subdirectory(data_load_ulog)
  add_subdirectory(data_load_mf4)
  add_subdirectory(data_load_blf)
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
  # data_stream_lsl is intentionally NOT in this aggregate list: liblsl requires
  # boost>=1.85, which conflicts with the boost/1.81.0 that Arrow's Flight thrift
  # pins here. It builds per-plugin against its own conanfile (like
  # data_load_lerobot/data_load_mp4).
  add_subdirectory(toolbox_fft)
  add_subdirectory(toolbox_data_exporter)
  add_subdirectory(toolbox_quaternion)
  add_subdirectory(toolbox_anomaly_detector)
  if(PJ_BUILD_ANOMALY_RUNNER)
    add_subdirectory(tools/anomaly_runner)
  endif()
  add_subdirectory(toolbox_mosaico)
endif()
