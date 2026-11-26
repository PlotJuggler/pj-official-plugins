# Shared Arrow helpers used by data_load_parquet and data_load_lerobot. Internal target.
#
# Skipped when Arrow is unavailable: builds of non-Arrow plugins (e.g. the ROS2 proxy job)
# still configure this subdir but have no Arrow dep. A real consumer does its own
# find_package(Arrow REQUIRED), so it still errors there.

if(NOT TARGET Arrow::arrow_static)
  find_package(Arrow QUIET CONFIG)
  if(NOT TARGET Arrow::arrow_static)
    message(STATUS "pj_arrow_helpers: Arrow not found, skipping shared helper library")
    return()
  endif()
endif()

add_library(pj_arrow_helpers STATIC arrow_helpers.cpp)
target_compile_features(pj_arrow_helpers PUBLIC cxx_std_20)
if(DEFINED PJ_WARNING_FLAGS)
  target_compile_options(pj_arrow_helpers PRIVATE ${PJ_WARNING_FLAGS})
endif()

# Linked into SHARED plugin .so files: must be position-independent.
set_target_properties(pj_arrow_helpers PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(pj_arrow_helpers PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Arrow types appear in the public API; plugin_sdk supplies PJ::sdk::ValueRef.
target_link_libraries(pj_arrow_helpers PUBLIC
  plotjuggler_sdk::plugin_sdk
  Arrow::arrow_static
)
