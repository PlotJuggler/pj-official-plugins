# WebAssembly build of pj_can_dbc (cmake/PjWasmPlugin.cmake), on the host's dbc_parser_cpp and
# fast_float source trees at this directory's pins (dbc_parser_cpp_SOURCE_DIR,
# dbc_fast_float_SOURCE_DIR).
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PjWasmPlugin.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/dbc_parser.cmake")

pj_add_dbc_parser_library(pj_wasm_dbc_parser "${dbc_parser_cpp_SOURCE_DIR}" "${dbc_fast_float_SOURCE_DIR}")
add_library(pj_can_dbc STATIC
  "${CMAKE_CURRENT_LIST_DIR}/can_decoder.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/can_topic.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/dictionary_paths.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/signal_row.cpp")
target_compile_features(pj_can_dbc PUBLIC cxx_std_20)
target_include_directories(pj_can_dbc PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(pj_can_dbc PUBLIC plotjuggler_sdk::base PRIVATE pj_wasm_dbc_parser)
pj_wasm_finalize_targets(pj_can_dbc)
