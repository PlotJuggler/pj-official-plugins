include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)

# data_tamer_parser — header-only library for DataTamer schema/snapshot parsing.
# Reuse cached download if parser_ros already fetched it.
CPMAddPackage(
  NAME data_tamer
  URL https://github.com/PickNikRobotics/data_tamer/archive/refs/tags/1.0.3.zip
  DOWNLOAD_ONLY YES
)
if(NOT TARGET data_tamer_parser)
  add_library(data_tamer_parser INTERFACE)
  target_include_directories(data_tamer_parser SYSTEM INTERFACE
    "${data_tamer_SOURCE_DIR}/data_tamer_cpp/include")
endif()

# --- Plugin shared library ---
add_library(parser_data_tamer_plugin SHARED data_tamer_parser.cpp)
target_compile_features(parser_data_tamer_plugin PRIVATE cxx_std_20)
target_compile_options(parser_data_tamer_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_data_tamer_plugin PRIVATE pj_base data_tamer_parser)

# --- Integration test ---
add_executable(data_tamer_parser_test tests/data_tamer_parser_test.cpp)
target_compile_features(data_tamer_parser_test PRIVATE cxx_std_20)
target_compile_options(data_tamer_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(data_tamer_parser_test PRIVATE
  PJ_DATA_TAMER_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_data_tamer_plugin>"
)
target_link_libraries(data_tamer_parser_test PRIVATE
  pj_base
  pj_message_parser_host
  data_tamer_parser
  GTest::gtest_main
)
add_test(NAME data_tamer_parser_test COMMAND data_tamer_parser_test)
