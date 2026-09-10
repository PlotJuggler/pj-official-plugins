# pj_grid_map — shared grid-map -> sdk::GridMap conversion. Transcodes the
# decoded parts of a grid_map_msgs/GridMap (column-major ring buffer) into an
# owned canonical GridMap, and finalizes/validates foxglove Grid views.
# Consumed by parser_ros and parser_protobuf. Qt-free; depends only on
# pj_base vocabulary types from the plugin SDK.

add_library(pj_grid_map STATIC grid_map_transcoder.cpp)
target_compile_features(pj_grid_map PUBLIC cxx_std_20)
if(DEFINED PJ_WARNING_FLAGS)
  target_compile_options(pj_grid_map PRIVATE ${PJ_WARNING_FLAGS})
endif()

# Static lib goes inside SHARED plugin .so files → must be position-independent.
set_target_properties(pj_grid_map PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(pj_grid_map PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(pj_grid_map PUBLIC
  plotjuggler_sdk::plugin_sdk
)

# --- Unit tests: index math, ring buffer, basic-layer NaN policy, rejections ---
find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(grid_map_transcoder_test tests/grid_map_transcoder_test.cpp)
  target_compile_features(grid_map_transcoder_test PRIVATE cxx_std_20)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(grid_map_transcoder_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(grid_map_transcoder_test PRIVATE pj_grid_map GTest::gtest_main)
  add_test(NAME grid_map_transcoder_test COMMAND grid_map_transcoder_test)
endif()
