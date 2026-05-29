find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# rosx_introspection builds tests by default; disable them.
set(INTROSPECTION_TESTS OFF CACHE BOOL "" FORCE)

# Prevent rosx_introspection from invoking ament_package() when ROS is installed
# on the host but we are not building inside a colcon workspace.
set(CMAKE_DISABLE_FIND_PACKAGE_ament_cmake TRUE)

CPMAddPackage(
  NAME rosx_introspection
  URL https://github.com/facontidavide/rosx_introspection/archive/refs/heads/master.zip
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

# --- Plugin shared library ---
add_library(parser_ros_plugin SHARED
  ros_parser.cpp
  ros_specializations.cpp
  ros_builtin_object_handlers.cpp
)
target_compile_features(parser_ros_plugin PRIVATE cxx_std_20)
target_compile_options(parser_ros_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_ros_plugin PRIVATE
  plotjuggler_core::plugin_sdk nlohmann_json::nlohmann_json rosx_introspection data_tamer_parser pj_config_utils)

pj_embed_manifest(parser_ros_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/ros_manifest.hpp
  VAR_NAME kRosManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(parser_ros_plugin
  FAMILY        message_parser
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Integration test (loads plugin .so via host library) ---
add_executable(ros_parser_test tests/ros_parser_test.cpp)
target_compile_features(ros_parser_test PRIVATE cxx_std_20)
target_compile_options(ros_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(ros_parser_test PRIVATE
  PJ_ROS_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_ros_plugin>"
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
