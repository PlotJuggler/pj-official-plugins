# Shared Arrow helpers used by data_load_parquet and data_load_lerobot.
# Internal target — not exported, not installed; consumers in this repo link
# pj_arrow_helpers directly.
#
# Skipped gracefully when Arrow is unavailable: builds that exercise only
# non-Arrow plugins (e.g. the ROS2 proxy job in ci-ros2.yml) still configure
# this subdir from the top-level CMakeLists.txt but have no Arrow Conan dep.
# Any plugin that actually consumes pj_arrow_helpers already does its own
# find_package(Arrow REQUIRED), so a real consumer never silently runs
# without the target — it errors at the consumer's own find_package.

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

# Static lib goes inside SHARED plugin .so files → must be position-independent.
set_target_properties(pj_arrow_helpers PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(pj_arrow_helpers PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Arrow types appear in the public API; plugin_sdk supplies PJ::sdk::ValueRef.
target_link_libraries(pj_arrow_helpers PUBLIC
  plotjuggler_sdk::plugin_sdk
  Arrow::arrow_static
)
