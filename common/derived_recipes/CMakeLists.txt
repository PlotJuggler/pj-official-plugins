# Shared, UI-free helpers for creating persisted derived recipes (transform / on_demand)
# through pj.data_processors.v1: path and input resolution, typed outputs, the generated Luau
# chunk and the display<->raw time conversion. Consumer: toolbox_assistant_agent. Internal target — not exported, not installed.

find_package(nlohmann_json REQUIRED)

add_library(pj_derived_recipes STATIC recipes.cpp)
target_compile_features(pj_derived_recipes PUBLIC cxx_std_20)
if(DEFINED PJ_WARNING_FLAGS)
  target_compile_options(pj_derived_recipes PRIVATE ${PJ_WARNING_FLAGS})
endif()

# Static lib goes inside SHARED plugin .so files → must be position-independent.
set_target_properties(pj_derived_recipes PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(pj_derived_recipes PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(pj_derived_recipes PUBLIC
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
)

if(TARGET GTest::gtest_main)
  add_executable(derived_recipes_test tests/recipes_test.cpp)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(derived_recipes_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(derived_recipes_test PRIVATE pj_derived_recipes GTest::gtest_main)
  add_test(NAME derived_recipes_test COMMAND derived_recipes_test)
endif()
