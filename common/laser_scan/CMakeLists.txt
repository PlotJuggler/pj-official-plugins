# pj_laser_scan — shared planar laser-scan -> sdk::PointCloud projection with
# a cached cos/sin LUT. Consumed by parser_ros (sensor_msgs/LaserScan) and
# parser_protobuf (foxglove.LaserScan). Qt-free; depends only on pj_base
# vocabulary types from the plugin SDK.

add_library(pj_laser_scan STATIC laser_scan_projector.cpp)
target_compile_features(pj_laser_scan PUBLIC cxx_std_20)
if(DEFINED PJ_WARNING_FLAGS)
  target_compile_options(pj_laser_scan PRIVATE ${PJ_WARNING_FLAGS})
endif()

# Static lib goes inside SHARED plugin .so files → must be position-independent.
set_target_properties(pj_laser_scan PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(pj_laser_scan PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# PJ::Span / sdk::PointCloud come from the SDK's base component.
target_link_libraries(pj_laser_scan PUBLIC
  plotjuggler_sdk::plugin_sdk
)

# --- Unit tests: LUT caching/invalidation + projection/drop/intensity layout ---
find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(laser_scan_projector_test tests/laser_scan_projector_test.cpp)
  target_compile_features(laser_scan_projector_test PRIVATE cxx_std_20)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(laser_scan_projector_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(laser_scan_projector_test PRIVATE pj_laser_scan GTest::gtest_main)
  add_test(NAME laser_scan_projector_test COMMAND laser_scan_projector_test)
endif()
