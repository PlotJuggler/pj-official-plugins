cmake_minimum_required(VERSION 3.22)

# The headless anomaly_runner (and its Luau layer anomaly_core + pj_scripting_core) is OFF by
# default: the GUI Marker Editor is Luau-free (the host runs the rules), so the plugin CI does not
# need the private pj_scripting_core package. Build the runner with
# `./build.sh tools/anomaly_runner` (its recipe provides pj_scripting_core) or
# -DPJ_BUILD_ANOMALY_RUNNER=ON.
option(PJ_BUILD_ANOMALY_RUNNER "Build the headless anomaly_runner CLI (needs pj_scripting_core)" OFF)

# Widget-family libraries (widget_basic) link Qt, which the aggregate build and its Conan closure do
# not provide: they build only with -DPJ_BUILD_WIDGETS=ON (and Qt on the prefix path), or alone via
# `./build.sh widget_basic`.
option(PJ_BUILD_WIDGETS "Build the Qt-linking Widget-family plugins (needs the official Qt)" OFF)

# Dual-mode: if plotjuggler_sdk::plugin_sdk already exists as a target, this is a subdirectory of
# plotjuggler_sdk and the parent provides all deps.
if(TARGET plotjuggler_sdk::plugin_sdk)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

  add_subdirectory(common/arrow_helpers)
  add_subdirectory(common/base64)
  add_subdirectory(common/utf8_path)
  add_subdirectory(common/pj_video_demux)
  add_subdirectory(common/websocket)
  add_subdirectory(common/always_included)
  add_subdirectory(common/laser_scan)
  add_subdirectory(common/grid_map)
  add_subdirectory(common/pointcloud_color)
  add_subdirectory(common/query)
  add_subdirectory(common/transfer_rate)
  add_subdirectory(common/toolbox_preview)
  add_subdirectory(common/derived_recipes)
  add_subdirectory(common/test_support)
  # This block adds every plugin, so can_dbc's consumers are always present; the standalone block
  # below gates it by PJ_BUILD_PLUGIN to avoid the CPM download for unrelated plugins.
  add_subdirectory(common/can_dbc)

  add_subdirectory(data_load_csv)
  add_subdirectory(data_load_mcap)
  add_subdirectory(data_load_parquet)
  add_subdirectory(data_load_ulog)
  add_subdirectory(data_load_mf4)
  add_subdirectory(data_load_blf)
  add_subdirectory(data_load_candump)
  add_subdirectory(data_load_3d)
  add_subdirectory(data_load_lerobot)
  add_subdirectory(data_load_mp4)
  add_subdirectory(parser_json)
  add_subdirectory(parser_protobuf)
  add_subdirectory(parser_ros)
  add_subdirectory(parser_data_tamer)
  add_subdirectory(parser_influx)
  add_subdirectory(parser_navmap)
  add_subdirectory(parser_arrow)
  add_subdirectory(data_stream_zmq)
  add_subdirectory(data_stream_mqtt)
  add_subdirectory(data_stream_udp)
  add_subdirectory(data_stream_foxglove_bridge)
  add_subdirectory(data_stream_webrtc)
  add_subdirectory(data_stream_pj_bridge)
  add_subdirectory(data_stream_dummy)
  add_subdirectory(data_stream_lsl)
  add_subdirectory(toolbox_fft)
  add_subdirectory(toolbox_data_exporter)
  add_subdirectory(toolbox_quaternion)
  add_subdirectory(toolbox_anomaly_detector)
  if(PJ_BUILD_ANOMALY_RUNNER)
    add_subdirectory(tools/anomaly_runner)
  endif()
  add_subdirectory(toolbox_mosaico)
  add_subdirectory(toolbox_transform_editor)
  add_subdirectory(toolbox_assistant_agent)
  if(PJ_BUILD_WIDGETS)
    add_subdirectory(widget_basic)
  endif()
  return()
endif()

project(pj_ported_plugins LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Sanitizer instrumentation (build.sh --asan) changes inlining and value-range propagation, which
# turns optimization-dependent warnings such as -Wmaybe-uninitialized into false positives (e.g.
# data_stream_pj_bridge's std::optional access under ASan). build.sh sets this option for that lane
# to demote -Werror wholesale; normal builds keep it OFF.
option(PJ_PLUGINS_NO_WERROR "Demote -Werror to non-fatal warnings (sanitizer builds only)" OFF)

if(MSVC)
  set(PJ_WARNING_FLAGS /W4 /WX /permissive-)
else()
  set(PJ_WARNING_FLAGS
    -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-qual -Wconversion -Woverloaded-virtual -Wpedantic
  )
  # GCC's -O3 inliner emits false stringop-overflow / array-bounds errors on std::vector<byte>
  # insert/append (e.g. data_stream_pj_bridge's buildTestFrame on GCC 13); demote them to warnings.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND PJ_WARNING_FLAGS -Wno-error=stringop-overflow -Wno-error=array-bounds)
  endif()
  if(PJ_PLUGINS_NO_WERROR)
    list(APPEND PJ_WARNING_FLAGS -Wno-error)
  endif()
endif()

# Convenience fallback; prefer -DCMAKE_TOOLCHAIN_FILE.
if(EXISTS "${CMAKE_BINARY_DIR}/conan_toolchain.cmake")
  include("${CMAKE_BINARY_DIR}/conan_toolchain.cmake")
endif()

find_package(GTest REQUIRED)

# The SDK version is pinned by SDK_VERSION through the conanfile.py recipes, not asserted here.
find_package(plotjuggler_sdk CONFIG REQUIRED
  COMPONENTS base plugin_sdk plugin_host
)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

enable_testing()

# enable_testing() does not define BUILD_TESTING (only include(CTest) does, which would pull in
# CTest's dashboard machinery), and plugins such as toolbox_mosaico guard their test tree with
# `if(BUILD_TESTING)`.
option(BUILD_TESTING "Build the testing tree" ON)

# Internal helper libraries shared by several plugins, added before any plugin subdir.
# common/arrow_helpers is configured in single-plugin mode ONLY for its consumers: configuring it
# pulls in the system Arrow CMake config, whose FindProtobufAlt re-includes whatever Protobuf is on
# CMAKE_PREFIX_PATH and collides with the Conan protobuf (6.x + abseil) a non-Arrow plugin such as
# parser_protobuf requires. The other helpers are safe to always configure.
set(PJ_ARROW_HELPER_CONSUMERS data_load_parquet data_load_lerobot)
if(DEFINED PJ_BUILD_PLUGIN)
  if(PJ_BUILD_PLUGIN IN_LIST PJ_ARROW_HELPER_CONSUMERS)
    add_subdirectory(common/arrow_helpers)
  endif()
else()
  add_subdirectory(common/arrow_helpers)
endif()
add_subdirectory(common/base64)
add_subdirectory(common/utf8_path)
add_subdirectory(common/pj_video_demux)
# Shared WebSocket transport and its tests: only for the websocket streamers.
set(PJ_WEBSOCKET_CONSUMERS data_stream_pj_bridge data_stream_foxglove_bridge)
if(NOT DEFINED PJ_BUILD_PLUGIN OR PJ_BUILD_PLUGIN IN_LIST PJ_WEBSOCKET_CONSUMERS)
  add_subdirectory(common/websocket)
endif()
# Header-only: channels that topic-selecting plugins (MCAP loader, streamers) always load, and the
# streamers' locked topic table.
add_subdirectory(common/always_included)
add_subdirectory(common/laser_scan)
add_subdirectory(common/grid_map)
add_subdirectory(common/pointcloud_color)
add_subdirectory(common/query)
add_subdirectory(common/transfer_rate)
# common/can_dbc downloads via CPM at configure time, so only its consumers configure it.
set(PJ_CAN_DBC_CONSUMERS data_load_mf4 data_load_blf data_load_candump)
if(NOT DEFINED PJ_BUILD_PLUGIN OR PJ_BUILD_PLUGIN IN_LIST PJ_CAN_DBC_CONSUMERS)
  add_subdirectory(common/can_dbc)
endif()

# Shared toolbox-preview engine: single-plugin builds add it only for the generator toolboxes.
set(PJ_TOOLBOX_PREVIEW_CONSUMERS toolbox_anomaly_detector)
if(DEFINED PJ_BUILD_PLUGIN)
  if(PJ_BUILD_PLUGIN IN_LIST PJ_TOOLBOX_PREVIEW_CONSUMERS)
    add_subdirectory(common/toolbox_preview)
  endif()
else()
  add_subdirectory(common/toolbox_preview)
endif()

# Shared derived-recipe helpers (path/input resolution, Luau chunk) and their unit-test fake hosts.
set(PJ_DERIVED_RECIPES_CONSUMERS toolbox_assistant_agent toolbox_transform_editor)
if(NOT DEFINED PJ_BUILD_PLUGIN OR PJ_BUILD_PLUGIN IN_LIST PJ_DERIVED_RECIPES_CONSUMERS)
  add_subdirectory(common/derived_recipes)
endif()
# Header-only fake hosts for unit tests (no external dependencies).
add_subdirectory(common/test_support)

# -DPJ_BUILD_PLUGIN=<dir> builds a single plugin (CI mode); omit it to build all plugins.
if(DEFINED PJ_BUILD_PLUGIN)
  add_subdirectory(${PJ_BUILD_PLUGIN})
else()
  add_subdirectory(parser_json)
  add_subdirectory(parser_protobuf)
  add_subdirectory(parser_ros)
  add_subdirectory(parser_data_tamer)
  add_subdirectory(parser_influx)
  add_subdirectory(parser_navmap)
  add_subdirectory(parser_arrow)
  add_subdirectory(data_load_csv)
  add_subdirectory(data_load_mcap)
  add_subdirectory(data_load_parquet)
  add_subdirectory(data_load_ulog)
  add_subdirectory(data_load_mf4)
  add_subdirectory(data_load_blf)
  add_subdirectory(data_load_candump)
  add_subdirectory(data_load_3d)
  add_subdirectory(data_load_lerobot)
  add_subdirectory(data_load_mp4)
  add_subdirectory(data_stream_zmq)
  add_subdirectory(data_stream_mqtt)
  add_subdirectory(data_stream_udp)
  add_subdirectory(data_stream_foxglove_bridge)
  add_subdirectory(data_stream_webrtc)
  add_subdirectory(data_stream_pj_bridge)
  add_subdirectory(data_stream_dummy)
  add_subdirectory(data_stream_lsl)
  add_subdirectory(toolbox_fft)
  add_subdirectory(toolbox_data_exporter)
  add_subdirectory(toolbox_quaternion)
  add_subdirectory(toolbox_anomaly_detector)
  if(PJ_BUILD_ANOMALY_RUNNER)
    add_subdirectory(tools/anomaly_runner)
  endif()
  add_subdirectory(toolbox_mosaico)
  add_subdirectory(toolbox_transform_editor)
  add_subdirectory(toolbox_assistant_agent)
  if(PJ_BUILD_WIDGETS)
    add_subdirectory(widget_basic)
  endif()
endif()
