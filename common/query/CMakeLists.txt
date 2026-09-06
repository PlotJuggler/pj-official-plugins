# pj_common_query — the metadata query language/editing core (lexer, parser,
# completion/assist analysis, visible-row filter) shared by toolbox providers.
# Header-only; Lua evaluation stays with each plugin behind the
# PJ::query::Evaluator seam. Depends only on pj_base (PJ::parseNumber,
# PJ::sdk::lowerAscii) from the plugin SDK.

add_library(pj_common_query INTERFACE)
target_include_directories(pj_common_query INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(pj_common_query INTERFACE cxx_std_20)
target_link_libraries(pj_common_query INTERFACE plotjuggler_sdk::base)

# --- Unit tests: lexer/parser/complete/analyze/apply + filter gating (fake evaluator) ---
find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  foreach(test_name query_test query_filter_test)
    add_executable(pj_common_${test_name} tests/${test_name}.cpp)
    target_compile_features(pj_common_${test_name} PRIVATE cxx_std_20)
    if(DEFINED PJ_WARNING_FLAGS)
      target_compile_options(pj_common_${test_name} PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    target_link_libraries(pj_common_${test_name} PRIVATE pj_common_query GTest::gtest_main)
    add_test(NAME pj_common_${test_name} COMMAND pj_common_${test_name})
  endforeach()
endif()
