# Two layers so the GUI plugin carries no script engine:
#  - anomaly_helpers: builtin rule library, JSON report, portable-rule (de)serialization.
#    Depends only on plugin_sdk (PlotMarker) and nlohmann_json. The toolbox plugin links this.
#  - anomaly_core: adds the Luau-backed runAnomalyScript on top of anomaly_helpers and the shared
#    pj_scripting_core. Only the headless runner links this.
# Static libs, PIC so anomaly_helpers can link into the SHARED plugin.

find_package(nlohmann_json REQUIRED)

add_library(anomaly_helpers STATIC anomaly_helpers.cpp)
target_compile_features(anomaly_helpers PUBLIC cxx_std_20)
target_include_directories(anomaly_helpers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(anomaly_helpers PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(anomaly_helpers PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(anomaly_helpers
  PUBLIC
    plotjuggler_sdk::plugin_sdk
  PRIVATE
    nlohmann_json::nlohmann_json
)

# Hangs off anomaly_helpers (not anomaly_core) so it runs wherever the plugin does, including the
# cross-platform plugin CI, which never sees the runner or pj_scripting_core.
if(TARGET GTest::gtest_main)
  add_executable(anomaly_helpers_test tests/anomaly_helpers_test.cpp)
  target_compile_options(anomaly_helpers_test PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(anomaly_helpers_test PRIVATE
    anomaly_helpers
    nlohmann_json::nlohmann_json
    GTest::gtest_main
  )
  add_test(NAME anomaly_helpers_test COMMAND anomaly_helpers_test)
endif()

# anomaly_core is built only for the headless runner (PJ_BUILD_ANOMALY_RUNNER, set by the runner's
# CMakeLists), so the default/aggregate build never needs pj_scripting_core.
if(PJ_BUILD_ANOMALY_RUNNER)
  find_package(pj_scripting_core REQUIRED)

  add_library(anomaly_core STATIC anomaly_core.cpp)
  target_compile_features(anomaly_core PUBLIC cxx_std_20)
  target_include_directories(anomaly_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(anomaly_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_options(anomaly_core PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(anomaly_core
    PUBLIC
      anomaly_helpers
      pj_scripting_core::pj_scripting_core
  )
endif()
