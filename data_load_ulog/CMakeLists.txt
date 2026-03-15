find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)

CPMAddPackage(
  NAME ulog_cpp
  URL https://github.com/PX4/ulog_cpp/archive/refs/tags/v1.0.1.zip
)

# Mark ulog_cpp include dirs as SYSTEM to suppress -Werror on
# third-party headers.
get_target_property(_ulog_includes ulog_cpp INTERFACE_INCLUDE_DIRECTORIES)
if(_ulog_includes)
  set_target_properties(ulog_cpp PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_ulog_includes}")
endif()

# --- Plugin shared library ---
add_library(ulog_source_plugin SHARED ulog_source.cpp)
target_compile_features(ulog_source_plugin PRIVATE cxx_std_20)
target_compile_options(ulog_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(ulog_source_plugin PRIVATE pj_base ulog_cpp::ulog_cpp nlohmann_json::nlohmann_json)

# --- Integration test ---
add_executable(ulog_source_test tests/ulog_source_test.cpp)
target_compile_features(ulog_source_test PRIVATE cxx_std_20)
target_compile_options(ulog_source_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(ulog_source_test PRIVATE ulog_cpp::ulog_cpp GTest::gtest_main)
add_test(NAME ulog_source_test COMMAND ulog_source_test)
