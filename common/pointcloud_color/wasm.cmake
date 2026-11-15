# WebAssembly build of pj_pointcloud_color (cmake/PjWasmPlugin.cmake): header-only.
include_guard(GLOBAL)

add_library(pj_pointcloud_color INTERFACE)
target_compile_features(pj_pointcloud_color INTERFACE cxx_std_20)
target_include_directories(pj_pointcloud_color INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(pj_pointcloud_color INTERFACE plotjuggler_sdk::plugin_sdk)
