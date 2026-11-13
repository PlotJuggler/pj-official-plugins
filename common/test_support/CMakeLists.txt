# Header-only fake hosts shared by the plugin unit tests (namespace toolbox_testing): a catalog-only
# toolbox host, playback and viewport hosts, a recording data-processors host and a widget host.
# Include them as "test_support/<name>.hpp". Internal target: not exported, not installed.
add_library(pj_test_support INTERFACE)
target_include_directories(pj_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
# plugin_host: fake_widget_host.hpp serves its history through the SDK's WidgetHostBridge.
target_link_libraries(pj_test_support INTERFACE plotjuggler_sdk::plugin_sdk plotjuggler_sdk::plugin_host)

if(TARGET GTest::gtest_main)
  add_executable(fake_widget_host_test tests/fake_widget_host_test.cpp)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(fake_widget_host_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(fake_widget_host_test PRIVATE pj_test_support GTest::gtest_main)
  add_test(NAME fake_widget_host_test COMMAND fake_widget_host_test)
endif()
