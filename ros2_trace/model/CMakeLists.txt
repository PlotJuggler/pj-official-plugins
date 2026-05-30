# ros2_trace_model — UI-agnostic ROS 2 trace decode/join engine.
# No PlotJuggler dependency, no Qt. libbabeltrace2 is only pulled in for the
# CTF reader (added later, guarded by ROS2_TRACE_WITH_BABELTRACE).
#
# Dual-mode: builds standalone (own project, for fast TDD of the join engine)
# or as an add_subdirectory of the pj-official-plugins tree.

cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED PROJECT_NAME)
  project(ros2_trace_model LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# Reuse the parent's warning flags when embedded; otherwise mirror them so the
# standalone build stays just as strict (keeps output pristine for the parent).
if(NOT DEFINED PJ_WARNING_FLAGS)
  if(MSVC)
    set(PJ_WARNING_FLAGS /W4 /WX /permissive-)
  else()
    set(PJ_WARNING_FLAGS
      -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Wold-style-cast
      -Wcast-qual -Wconversion -Woverloaded-virtual -Wpedantic
    )
  endif()
endif()

add_library(ros2_trace_model STATIC
  src/raw_event.cpp
  src/registry.cpp
  src/callback_deriver.cpp
  src/pipeline.cpp
)
set_target_properties(ros2_trace_model PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(ros2_trace_model PUBLIC cxx_std_20)
target_compile_options(ros2_trace_model PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(ros2_trace_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

include(CTest)
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  add_executable(ros2_trace_model_test
    tests/registry_test.cpp
    tests/callback_deriver_test.cpp
    tests/pipeline_test.cpp
  )
  target_compile_features(ros2_trace_model_test PRIVATE cxx_std_20)
  target_compile_options(ros2_trace_model_test PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(ros2_trace_model_test PRIVATE ros2_trace_model GTest::gtest_main)
  add_test(NAME ros2_trace_model_test COMMAND ros2_trace_model_test)
endif()
