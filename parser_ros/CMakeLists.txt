find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)

# rosx_introspection builds tests by default; disable them.
set(INTROSPECTION_TESTS OFF CACHE BOOL "" FORCE)

CPMAddPackage(
  NAME rosx_introspection
  URL https://github.com/facontidavide/rosx_introspection/archive/refs/heads/master.zip
)

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
add_library(parser_ros_plugin SHARED ros_parser.cpp)
target_compile_features(parser_ros_plugin PRIVATE cxx_std_20)
target_compile_options(parser_ros_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_ros_plugin PRIVATE
  pj_base nlohmann_json::nlohmann_json rosx_introspection data_tamer_parser)

# --- Integration test (loads plugin .so via host library) ---
add_executable(ros_parser_test tests/ros_parser_test.cpp)
target_compile_features(ros_parser_test PRIVATE cxx_std_20)
target_compile_options(ros_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(ros_parser_test PRIVATE
  PJ_ROS_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_ros_plugin>"
)
target_link_libraries(ros_parser_test PRIVATE
  pj_base
  pj_message_parser_host
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
    pj_base pj_message_parser_host rosx_introspection benchmark::benchmark)
endif()
