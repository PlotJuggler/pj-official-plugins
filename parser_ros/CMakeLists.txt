find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# Prevent rosx_introspection from invoking ament_package() when ROS is installed
# on the host but we are not building inside a colcon workspace.
set(CMAKE_DISABLE_FIND_PACKAGE_ament_cmake TRUE)

# Pin rosx_introspection 3.x for IDL-support APIs used by the PJ3 ROS parser lineage.
CPMAddPackage(
  NAME rosx_introspection
  URL https://github.com/facontidavide/rosx_introspection/archive/refs/tags/3.0.0.zip
  URL_HASH SHA256=726b3d42a574659e6b793ca2b64a2691d7cf0149f5fd3d0fa8edf7e8b8413e9d
  OPTIONS "ROSX_HAS_JSON OFF"
)

# Ensure rosx_introspection is statically linked into the plugin .so.
set_target_properties(rosx_introspection PROPERTIES POSITION_INDEPENDENT_CODE ON)

# rosx_introspection bundles peglib.h, which requires __cplusplus >= 201703L.
# MSVC reports 199711L unless /Zc:__cplusplus is set, so the Windows build
# otherwise fails with C1189 "Requires complete C++17 support".
target_compile_options(rosx_introspection PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>)

# Mark rosx_introspection include dirs as SYSTEM to suppress -Werror on
# third-party headers (old-style casts, narrowing conversions, etc.).
get_target_property(_rosx_includes rosx_introspection INTERFACE_INCLUDE_DIRECTORIES)
if(_rosx_includes)
  set_target_properties(rosx_introspection PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_rosx_includes}")
endif()

# data_tamer_parser — header-only library for DataTamer schema/snapshot parsing.
CPMAddPackage(
  NAME data_tamer
  URL https://github.com/PickNikRobotics/data_tamer/archive/refs/tags/1.0.3.zip
  DOWNLOAD_ONLY YES
)
add_library(data_tamer_parser INTERFACE)
target_include_directories(data_tamer_parser SYSTEM INTERFACE
  "${data_tamer_SOURCE_DIR}/data_tamer_cpp/include")

set(PARSER_ROS_COMMON_SOURCES
  ros_parser.cpp
  ros_specializations.cpp
  ros_builtin_object_handlers.cpp
)

function(configure_parser_ros_target TARGET)
  target_compile_features(${TARGET} PRIVATE cxx_std_20)
  target_compile_options(${TARGET} PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(${TARGET} PRIVATE
    plotjuggler_core::plugin_sdk nlohmann_json::nlohmann_json rosx_introspection data_tamer_parser)
endfunction()

# --- Plugin shared libraries ---
add_library(parser_ros_plugin SHARED
  ${PARSER_ROS_COMMON_SOURCES}
  ros_parser_plugin.cpp
)
configure_parser_ros_target(parser_ros_plugin)

pj_embed_manifest(parser_ros_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/ros_manifest.hpp
  VAR_NAME kRosManifest
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/rosmsg_manifest.json
)

pj_embed_ui(parser_ros_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ros_parser_options.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/ros_parser_options_ui.hpp
  VAR_NAME kRosParserOptionsUi
)

# v4 plugin discovery: emit sidecar .pjmanifest.json files the host can scan
# pre-dlopen. Content derived from each embedded parser manifest + {abi_major, family}.
pj_emit_plugin_manifest(parser_ros_plugin
  FAMILY        message_parser
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/rosmsg_manifest.json
)

add_library(parser_ros_omgidl_plugin SHARED
  ${PARSER_ROS_COMMON_SOURCES}
  ros_omgidl_parser_plugin.cpp
)
configure_parser_ros_target(parser_ros_omgidl_plugin)

pj_embed_manifest(parser_ros_omgidl_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/ros_omgidl_manifest.hpp
  VAR_NAME kRosOmgIdlManifest
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/omgidl_manifest.json
)

pj_emit_plugin_manifest(parser_ros_omgidl_plugin
  FAMILY        message_parser
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/omgidl_manifest.json
)

# --- Integration test (loads plugin .so via host library) ---
add_executable(ros_parser_test tests/ros_parser_test.cpp)
target_compile_features(ros_parser_test PRIVATE cxx_std_20)
target_compile_options(ros_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(ros_parser_test PRIVATE
  PJ_ROS_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_ros_plugin>"
  PJ_ROS_IDL_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_ros_omgidl_plugin>"
)
target_link_libraries(ros_parser_test PRIVATE
  plotjuggler_core::plugin_sdk
  plotjuggler_core::plugin_host
  rosx_introspection
  nlohmann_json::nlohmann_json
  GTest::gtest_main
)
add_test(NAME ros_parser_test COMMAND ros_parser_test)

# --- Benchmark (optional, only if benchmark library is available) ---
find_package(benchmark QUIET)
if(benchmark_FOUND)
  add_executable(ros_parser_bench benchmarks/ros_parser_bench.cpp)
  target_compile_features(ros_parser_bench PRIVATE cxx_std_20)
  target_compile_definitions(ros_parser_bench PRIVATE
    PJ_ROS_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_ros_plugin>"
  )
  target_link_libraries(ros_parser_bench PRIVATE
    plotjuggler_core::plugin_sdk plotjuggler_core::plugin_host rosx_introspection benchmark::benchmark)
endif()
