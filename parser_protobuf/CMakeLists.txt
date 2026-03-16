find_package(nlohmann_json REQUIRED)
find_package(protobuf REQUIRED)

# --- Plugin shared library ---
add_library(parser_protobuf_plugin SHARED protobuf_parser.cpp)
target_compile_features(parser_protobuf_plugin PRIVATE cxx_std_20)
target_compile_options(parser_protobuf_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_protobuf_plugin PRIVATE pj_base protobuf::protobuf nlohmann_json::nlohmann_json)

# --- Integration test (loads plugin .so via host library) ---
add_executable(protobuf_parser_test tests/protobuf_parser_test.cpp)
target_compile_features(protobuf_parser_test PRIVATE cxx_std_20)
target_compile_options(protobuf_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(protobuf_parser_test PRIVATE
  PJ_PROTOBUF_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_protobuf_plugin>"
)
target_link_libraries(protobuf_parser_test PRIVATE
  pj_base
  pj_message_parser_host
  protobuf::protobuf
  GTest::gtest_main
)
add_test(NAME protobuf_parser_test COMMAND protobuf_parser_test)
