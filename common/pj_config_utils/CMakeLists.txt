# Shared JSON config-parsing helpers implementing the two-tier parse-error
# policy (parseStrict for data sources, parseLenient for parsers). Internal
# target — not exported, not installed; consumers in this repo link
# pj_config_utils directly.
#
# Only depends on nlohmann_json (public API) and the SDK's PJ::Expected, both
# always available in every build mode, so — unlike pj_arrow_helpers — there
# is no optional dependency to gate on.

add_library(pj_config_utils STATIC config_utils.cpp)
target_compile_features(pj_config_utils PUBLIC cxx_std_20)
if(DEFINED PJ_WARNING_FLAGS)
  target_compile_options(pj_config_utils PRIVATE ${PJ_WARNING_FLAGS})
endif()

# Static lib goes inside SHARED plugin .so files → must be position-independent.
set_target_properties(pj_config_utils PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(pj_config_utils PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# nlohmann::json appears in the public API; plugin_sdk supplies PJ::Expected.
target_link_libraries(pj_config_utils PUBLIC
  plotjuggler_core::plugin_sdk
  nlohmann_json::nlohmann_json
)

# Unit test — only when GTest is available (standalone build wires it via the
# root CMakeLists; subdirectory builds inside plotjuggler_core also provide it).
if(TARGET GTest::gtest_main)
  add_executable(pj_config_utils_test tests/config_utils_test.cpp)
  target_compile_features(pj_config_utils_test PRIVATE cxx_std_20)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_config_utils_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(pj_config_utils_test PRIVATE pj_config_utils GTest::gtest_main)
  add_test(NAME pj_config_utils_test COMMAND pj_config_utils_test)
endif()
