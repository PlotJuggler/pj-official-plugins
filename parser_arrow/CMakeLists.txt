find_package(nanoarrow REQUIRED)
find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# --- Plugin shared library ---
add_library(parser_arrow_plugin SHARED src/arrow_parser.cpp)
target_compile_features(parser_arrow_plugin PRIVATE cxx_std_20)
target_compile_options(parser_arrow_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_arrow_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  nanoarrow::nanoarrow
  nanoarrow::nanoarrow_ipc
  nlohmann_json::nlohmann_json
)

pj_embed_manifest(parser_arrow_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/arrow_manifest.hpp
  VAR_NAME kArrowManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(parser_arrow_plugin
  FAMILY        message_parser
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Integration test (loads plugin .so via host library) ---
add_executable(arrow_parser_test tests/arrow_parser_test.cpp)
target_compile_features(arrow_parser_test PRIVATE cxx_std_20)
target_compile_options(arrow_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(arrow_parser_test PRIVATE
  PJ_ARROW_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_arrow_plugin>"
)
target_link_libraries(arrow_parser_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  plotjuggler_sdk::plugin_host
  GTest::gtest_main
)
add_test(NAME arrow_parser_test COMMAND arrow_parser_test)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(parser_arrow_plugin
  REQUIRED_EXPORTS PJ_get_message_parser_vtable)
