# WebAssembly build of pj_base64 (cmake/PjWasmPlugin.cmake): header-only.
include_guard(GLOBAL)

add_library(pj_base64 INTERFACE)
target_include_directories(pj_base64 INTERFACE "${CMAKE_CURRENT_LIST_DIR}/include")
