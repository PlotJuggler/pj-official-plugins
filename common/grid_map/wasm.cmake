# WebAssembly build of pj_grid_map (cmake/PjWasmPlugin.cmake).
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PjWasmPlugin.cmake")

add_library(pj_grid_map STATIC "${CMAKE_CURRENT_LIST_DIR}/grid_map_transcoder.cpp")
target_compile_features(pj_grid_map PUBLIC cxx_std_20)
target_include_directories(pj_grid_map PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(pj_grid_map PUBLIC plotjuggler_sdk::plugin_sdk)
pj_wasm_finalize_targets(pj_grid_map)
