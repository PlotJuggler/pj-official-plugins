# pj_pointcloud_color — Qt-free canonical point-cloud colour normalization shared by
# parser_ros and parser_protobuf, so both emit the same canonical packed "rgba"/"rgb"
# colour field. Header-only; depends only on pj_base vocabulary from the plugin SDK.

add_library(pj_pointcloud_color INTERFACE)
target_compile_features(pj_pointcloud_color INTERFACE cxx_std_20)
target_include_directories(pj_pointcloud_color INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(pj_pointcloud_color INTERFACE
  plotjuggler_sdk::plugin_sdk
)

# --- Unit tests: separate-channel collapse, no-alpha, non-contiguous edge cases ---
find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(pointcloud_color_test tests/pointcloud_color_test.cpp)
  target_compile_features(pointcloud_color_test PRIVATE cxx_std_20)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pointcloud_color_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(pointcloud_color_test PRIVATE pj_pointcloud_color GTest::gtest_main)
  add_test(NAME pointcloud_color_test COMMAND pointcloud_color_test)
endif()
