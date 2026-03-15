find_package(nlohmann_json REQUIRED)

# --- Plugin shared library ---
add_library(parser_json_plugin SHARED json_parser.cpp)
target_compile_features(parser_json_plugin PRIVATE cxx_std_20)
target_compile_options(parser_json_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_json_plugin PRIVATE pj_base nlohmann_json::nlohmann_json)

# --- Integration test (loads plugin .so via host library) ---
add_executable(json_parser_test tests/json_parser_test.cpp)
target_compile_features(json_parser_test PRIVATE cxx_std_20)
target_compile_options(json_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(json_parser_test PRIVATE
  PJ_JSON_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_json_plugin>"
)
target_link_libraries(json_parser_test PRIVATE
  pj_base
  pj_message_parser_host
  GTest::gtest_main
)
add_test(NAME json_parser_test COMMAND json_parser_test)
