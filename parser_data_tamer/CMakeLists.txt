include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# data_tamer_parser — header-only library for DataTamer schema/snapshot parsing.
# data_tamer 2.0.0 (tag 2.0.0, commit 0c48ed0). Its own CPM name: parser_ros pins data_tamer 1.x
# as `data_tamer`, and CPM reuses the first package of a name, so in the aggregate build this
# plugin would get parser_ros's 1.x sources.
CPMAddPackage(
  NAME data_tamer_2
  URL https://github.com/facontidavide/data_tamer/archive/refs/tags/2.0.0.zip
  URL_HASH SHA256=0a8caef329d641b2cc72fa68211af37a36759614807add4bdba2ed2bf2fc8963
  DOWNLOAD_ONLY YES
)
add_library(parser_data_tamer_headers INTERFACE)
target_include_directories(parser_data_tamer_headers SYSTEM INTERFACE
  "${data_tamer_2_SOURCE_DIR}/data_tamer_cpp/include")

# --- Plugin shared library ---
add_library(parser_data_tamer_plugin SHARED data_tamer_parser.cpp)
target_compile_features(parser_data_tamer_plugin PRIVATE cxx_std_20)
target_compile_options(parser_data_tamer_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_data_tamer_plugin PRIVATE plotjuggler_sdk::plugin_sdk parser_data_tamer_headers)

pj_embed_manifest(parser_data_tamer_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/data_tamer_manifest.hpp
  VAR_NAME kDataTamerManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(parser_data_tamer_plugin
  FAMILY        message_parser
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Integration test ---
add_executable(data_tamer_parser_test tests/data_tamer_parser_test.cpp)
target_compile_features(data_tamer_parser_test PRIVATE cxx_std_20)
target_compile_options(data_tamer_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(data_tamer_parser_test PRIVATE
  PJ_DATA_TAMER_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_data_tamer_plugin>"
  PJ_DATA_TAMER_VECTORS_DIR="${data_tamer_2_SOURCE_DIR}/docs/wire_format/vectors"
)
target_link_libraries(data_tamer_parser_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  plotjuggler_sdk::plugin_host
  parser_data_tamer_headers
  GTest::gtest_main
)
add_test(NAME data_tamer_parser_test COMMAND data_tamer_parser_test)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(parser_data_tamer_plugin
  REQUIRED_EXPORTS PJ_get_message_parser_vtable)
