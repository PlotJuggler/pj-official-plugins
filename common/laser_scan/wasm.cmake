# WebAssembly build of pj_laser_scan (cmake/PjWasmPlugin.cmake).
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PjWasmPlugin.cmake")

add_library(pj_laser_scan STATIC "${CMAKE_CURRENT_LIST_DIR}/laser_scan_projector.cpp")
target_compile_features(pj_laser_scan PUBLIC cxx_std_20)
target_include_directories(pj_laser_scan PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(pj_laser_scan PUBLIC plotjuggler_sdk::plugin_sdk)
pj_wasm_finalize_targets(pj_laser_scan)
