include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# data_tamer_parser — header-only library for DataTamer schema/snapshot parsing.
# A main commit of data_tamer 2.x: switch to the 2.0.0 tag once it is released.
CPMAddPackage(
  NAME data_tamer
  URL https://github.com/facontidavide/data_tamer/archive/7b4dc8244ca29b7d609453f82843dd2f947a019b.zip
  URL_HASH SHA256=cad8e213bae7f8fb686ab3fef5b49e8161b60b4e765bbfd76f6712eda25bcdd2
  DOWNLOAD_ONLY YES
)
add_library(parser_data_tamer_headers INTERFACE)
target_include_directories(parser_data_tamer_headers SYSTEM INTERFACE
  "${data_tamer_SOURCE_DIR}/data_tamer_cpp/include")

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
  PJ_DATA_TAMER_VECTORS_DIR="${data_tamer_SOURCE_DIR}/docs/wire_format/vectors"
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
