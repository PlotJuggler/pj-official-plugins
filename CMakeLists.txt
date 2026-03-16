cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# Dual-mode: standalone project OR subdirectory of plotjuggler_core.
#
# Detection: if pj_base already exists as a target, we are a subdirectory
# and all deps are already provided by the parent project.
# ---------------------------------------------------------------------------

if(TARGET pj_base)
  # Subdirectory mode — deps already provided by parent.
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

  add_subdirectory(parser_json)
  add_subdirectory(parser_protobuf)
  add_subdirectory(parser_ros)
  add_subdirectory(parser_data_tamer)
  add_subdirectory(data_load_csv)
  add_subdirectory(data_load_mcap)
  add_subdirectory(data_load_parquet)
  add_subdirectory(data_load_ulog)
  add_subdirectory(data_stream_zmq)
  add_subdirectory(data_stream_mqtt)
  add_subdirectory(data_stream_foxglove_bridge)
  add_subdirectory(data_stream_pj_bridge)
  add_subdirectory(data_stream_dummy)
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
# Compiler warnings (matches plotjuggler_core)
# ---------------------------------------------------------------------------

if(MSVC)
  set(PJ_WARNING_FLAGS /W4 /WX /permissive-)
else()
  set(PJ_WARNING_FLAGS
    -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-qual -Wconversion -Woverloaded-virtual -Wpedantic
  )
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
# PlotJuggler Core SDK (via CPM)
# ---------------------------------------------------------------------------

include(cmake/CPM.cmake)

CPMAddPackage(
  NAME plotjuggler_core
  GIT_REPOSITORY git@github.com:PlotJuggler/plotjuggler_core.git
  GIT_TAG development
  OPTIONS
    "PJ_BUILD_DATASTORE OFF"
    "PJ_BUILD_PORTED_PLUGINS OFF"
    "PJ_BUILD_TESTS OFF"
    "PJ_BUILD_DIALOG_ENGINE_QT OFF"
    "PJ_BUILD_PARQUET_IMPORT_EXAMPLE OFF"
)

# ---------------------------------------------------------------------------
# Plugin output directory and testing
# ---------------------------------------------------------------------------

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

enable_testing()

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
  add_subdirectory(data_stream_zmq)
  add_subdirectory(data_stream_mqtt)
  add_subdirectory(data_stream_foxglove_bridge)
  add_subdirectory(data_stream_pj_bridge)
  add_subdirectory(data_stream_dummy)
endif()
