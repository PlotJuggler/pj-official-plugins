# WebAssembly build of pj_toolbox_preview (cmake/PjWasmPlugin.cmake).
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PjWasmPlugin.cmake")

add_library(pj_toolbox_preview STATIC
  "${CMAKE_CURRENT_LIST_DIR}/series_catalog.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/series_types.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/ephemeral_preview.cpp")
target_compile_features(pj_toolbox_preview PUBLIC cxx_std_20)
target_include_directories(pj_toolbox_preview PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(pj_toolbox_preview PUBLIC plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json)
pj_wasm_finalize_targets(pj_toolbox_preview)
