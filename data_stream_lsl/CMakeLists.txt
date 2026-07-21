find_package(LSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)

# --- Unit tests for the pure conversion helpers ---
add_executable(lsl_source_test tests/lsl_source_test.cpp)
target_compile_features(lsl_source_test PRIVATE cxx_std_20)
target_include_directories(lsl_source_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lsl_source_test PRIVATE
  plotjuggler_sdk::plugin_host LSL::lsl nlohmann_json::nlohmann_json GTest::gtest_main
)
add_test(NAME lsl_source_test COMMAND lsl_source_test)
