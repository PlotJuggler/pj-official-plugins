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

# --- Plugin shared library ---
add_library(parser_ros_plugin SHARED src/ros_parser.cpp)
target_compile_features(parser_ros_plugin PRIVATE cxx_std_20)
target_compile_options(parser_ros_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_ros_plugin PRIVATE pj_base nlohmann_json::nlohmann_json rosx_introspection)

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
  GTest::gtest_main
)
add_test(NAME ros_parser_test COMMAND ros_parser_test)
