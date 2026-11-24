# pj_always_included: the (schema, encoding) table of channels that topic-selecting plugins
# (data_load_mcap and the ROS 2 / PlotJuggler Bridge / Foxglove Bridge streamers) always load or
# subscribe, the tooltip of their locked rows, and the streamers' locked topic table
# (locked_table.hpp). Header-only.

add_library(pj_always_included INTERFACE)
target_include_directories(pj_always_included INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pj_always_included INTERFACE cxx_std_20)
target_link_libraries(pj_always_included INTERFACE plotjuggler_sdk::plugin_sdk)

find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  find_package(nlohmann_json REQUIRED)
  add_executable(pj_always_included_test tests/always_included_test.cpp tests/locked_table_test.cpp)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_always_included_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  # test_support/widget_table.hpp, header-only: included by path, since common/test_support is
  # configured only for some plugins.
  target_include_directories(pj_always_included_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(pj_always_included_test PRIVATE
    pj_always_included nlohmann_json::nlohmann_json GTest::gtest_main
  )
  add_test(NAME pj_always_included_test COMMAND pj_always_included_test)
endif()
