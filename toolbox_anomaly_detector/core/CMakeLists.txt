# Two layers so the GUI plugin can be Luau-free:
#
#  - anomaly_helpers — the GUI-free, ENGINE-free helpers (builtin rule library, JSON
#    report, portable-rule (de)serialization). Depends only on plugin_sdk (PlotMarker)
#    + nlohmann_json — NOT pj_scripting_core. The toolbox plugin links THIS, so it
#    carries no script engine: it submits rules to the host, which executes them.
#  - anomaly_core — adds the Luau-backed runAnomalyScript on top of anomaly_helpers +
#    the shared pj_scripting_core. The headless CLI runner links this (it executes
#    rules locally). Both paths run rules through the same engine -> GUI == headless.
#
# Static libs (PIC, so anomaly_helpers can link into the SHARED plugin).

find_package(nlohmann_json REQUIRED)

add_library(anomaly_helpers STATIC anomaly_helpers.cpp)
target_compile_features(anomaly_helpers PUBLIC cxx_std_20)
target_include_directories(anomaly_helpers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(anomaly_helpers PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(anomaly_helpers PRIVATE ${PJ_WARNING_FLAGS})
# plugin_sdk (PlotMarker) is PUBLIC; nlohmann_json is a .cpp implementation detail.
target_link_libraries(anomaly_helpers
  PUBLIC
    plotjuggler_sdk::plugin_sdk
  PRIVATE
    nlohmann_json::nlohmann_json
)

# Unit tests for the engine-free layer. Deliberately hangs off anomaly_helpers (NOT
# anomaly_core), so it builds and runs wherever the GUI plugin does — including the
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

# anomaly_core (the Luau engine layer) is built ONLY for the headless anomaly_runner
# (gated by PJ_BUILD_ANOMALY_RUNNER, which the runner's CMakeLists enables). The GUI
# plugin links anomaly_helpers and stays Luau-free, so the default/aggregate build never
# needs pj_scripting_core — that is what keeps the cross-platform plugin CI green.
if(PJ_BUILD_ANOMALY_RUNNER)
  find_package(pj_scripting_core REQUIRED)

  add_library(anomaly_core STATIC anomaly_core.cpp)
  target_compile_features(anomaly_core PUBLIC cxx_std_20)
  target_include_directories(anomaly_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(anomaly_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_options(anomaly_core PRIVATE ${PJ_WARNING_FLAGS})
  # anomaly_helpers (re-exported helpers) + pj_scripting_core (the engine that
  # runAnomalyScript delegates to) are PUBLIC, so the runner gets both.
  target_link_libraries(anomaly_core
    PUBLIC
      anomaly_helpers
      pj_scripting_core::pj_scripting_core
  )
endif()
