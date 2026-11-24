# pj_always_included — the (schema, encoding) table of channels that topic-selecting plugins
# (data_load_mcap and the ROS 2 / PlotJuggler Bridge / Foxglove Bridge streamers) always load or
# subscribe, and the tooltip of their locked rows. Header-only, no dependencies.

add_library(pj_always_included INTERFACE)
target_include_directories(pj_always_included INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pj_always_included INTERFACE cxx_std_20)

find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(pj_always_included_test tests/always_included_test.cpp)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_always_included_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(pj_always_included_test PRIVATE pj_always_included GTest::gtest_main)
  add_test(NAME pj_always_included_test COMMAND pj_always_included_test)
endif()
