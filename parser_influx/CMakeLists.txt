include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# --- Plugin shared library ---
add_library(parser_influx_plugin SHARED influx_parser.cpp)
target_compile_features(parser_influx_plugin PRIVATE cxx_std_20)
target_compile_options(parser_influx_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_influx_plugin PRIVATE plotjuggler_sdk::plugin_sdk)

pj_embed_manifest(parser_influx_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/influx_manifest.hpp
  VAR_NAME kInfluxManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(parser_influx_plugin
  FAMILY        message_parser
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Integration test (loads plugin .so via host library) ---
add_executable(influx_parser_test tests/influx_parser_test.cpp)
target_compile_features(influx_parser_test PRIVATE cxx_std_20)
target_compile_options(influx_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(influx_parser_test PRIVATE
  PJ_INFLUX_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_influx_plugin>"
)
target_link_libraries(influx_parser_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  plotjuggler_sdk::plugin_host
  GTest::gtest_main
)
add_test(NAME influx_parser_test COMMAND influx_parser_test)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(parser_influx_plugin
  REQUIRED_EXPORTS PJ_get_message_parser_vtable)
