find_package(nlohmann_json REQUIRED)
find_package(protobuf REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# --- Plugin shared library ---
add_library(parser_protobuf_plugin SHARED protobuf_parser.cpp foxglove_pointcloud_codec.cpp foxglove_object_codecs.cpp)
target_compile_features(parser_protobuf_plugin PRIVATE cxx_std_20)
target_compile_options(parser_protobuf_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_protobuf_plugin PRIVATE plotjuggler_sdk::plugin_sdk protobuf::libprotobuf protobuf::libprotoc nlohmann_json::nlohmann_json pj_base64 pj_array_policy pj_laser_scan)

pj_embed_ui(parser_protobuf_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/protobuf_parser_options.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/protobuf_parser_options_ui.hpp
  VAR_NAME kProtobufParserOptionsUi
)

pj_embed_manifest(parser_protobuf_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/protobuf_manifest.hpp
  VAR_NAME kProtobufManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(parser_protobuf_plugin
  FAMILY        message_parser
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# The object route returns sdk::BuiltinObject (std::any) through the in-process
# C++ plugin pointer. On macOS, hidden RTTI in the dylib gives libc++ distinct
# type identities for the concrete builtin structs (e.g. sdk::VideoFrame), so
# std::any_cast in the host/test executable fails even though the parser
# returned the right object. Keep the ELF isolation defaults from
# pj_emit_plugin_manifest, but let Darwin export C++ RTTI for this parser plugin.
if(APPLE)
  set_target_properties(parser_protobuf_plugin PROPERTIES
    CXX_VISIBILITY_PRESET default
    C_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
  )
endif()

# --- Integration test (loads plugin .so via host library) ---
add_executable(protobuf_parser_test tests/protobuf_parser_test.cpp foxglove_pointcloud_codec.cpp foxglove_object_codecs.cpp)
target_compile_features(protobuf_parser_test PRIVATE cxx_std_20)
target_compile_options(protobuf_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(protobuf_parser_test PRIVATE
  PJ_PROTOBUF_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_protobuf_plugin>"
)
target_link_libraries(protobuf_parser_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  plotjuggler_sdk::plugin_host
  protobuf::protobuf
  pj_laser_scan
  GTest::gtest_main
)
add_test(NAME protobuf_parser_test COMMAND protobuf_parser_test)
