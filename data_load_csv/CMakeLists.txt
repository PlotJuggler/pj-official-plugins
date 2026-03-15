find_package(nlohmann_json REQUIRED)
find_package(date REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

# --- CSV parser library (pure C++, no Qt) ---
add_library(csv_parser_lib STATIC
  src/csv_parser.cpp
  src/timestamp_parsing.cpp
)
set_target_properties(csv_parser_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(csv_parser_lib PUBLIC cxx_std_20)
target_compile_options(csv_parser_lib PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(csv_parser_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(csv_parser_lib PUBLIC date::date)

# --- Plugin shared library ---
add_library(csv_source_plugin SHARED src/csv_source.cpp)
target_compile_features(csv_source_plugin PRIVATE cxx_std_20)
target_compile_options(csv_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(csv_source_plugin PRIVATE
  pj_base pj_dialog_sdk csv_parser_lib nlohmann_json::nlohmann_json
)

pj_embed_ui(csv_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ui/dataload_csv.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/dataload_csv_ui.hpp
  VAR_NAME kDataLoadCsvUi
)

pj_embed_ui(csv_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ui/datetimehelp.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/datetimehelp_ui.hpp
  VAR_NAME kDateTimeHelpUi
)

# --- Unit test for CSV parser (no plugin loading) ---
add_executable(csv_parser_test tests/csv_parser_test.cpp)
target_compile_features(csv_parser_test PRIVATE cxx_std_20)
target_compile_options(csv_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(csv_parser_test PRIVATE csv_parser_lib GTest::gtest_main)
add_test(NAME csv_parser_test COMMAND csv_parser_test)
