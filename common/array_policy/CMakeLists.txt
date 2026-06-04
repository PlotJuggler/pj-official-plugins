# Shared array-size policy contract used by every parser/stream plugin in
# this repo. Header-only; lives in pj-official-plugins because no component
# of plotjuggler_sdk consumes it. Third-party plugins only need to honor the
# JSON keys documented in array_policy.hpp.

find_package(nlohmann_json CONFIG REQUIRED)

add_library(pj_array_policy INTERFACE)
target_include_directories(pj_array_policy INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(pj_array_policy INTERFACE
  nlohmann_json::nlohmann_json
)

add_executable(array_policy_test tests/array_policy_test.cpp)
if(DEFINED PJ_WARNING_FLAGS)
  target_compile_options(array_policy_test PRIVATE ${PJ_WARNING_FLAGS})
endif()
target_link_libraries(array_policy_test PRIVATE
  pj_array_policy
  GTest::gtest_main
)
add_test(NAME array_policy_test COMMAND array_policy_test)
