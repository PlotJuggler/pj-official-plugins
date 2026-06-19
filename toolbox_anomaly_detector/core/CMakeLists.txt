# anomaly_core — GUI-free helper lib shared by the toolbox plugin and the headless
# CLI runner: the builtin rule library, the JSON report, and portable-rule
# (de)serialization. The detection ENGINE now lives in the shared pj_scripting_core
# (Luau); runAnomalyScript delegates to it, so a rule runs identically GUI <->
# headless. Static lib (PIC, so it can link into the SHARED plugin).

find_package(pj_scripting_core REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(anomaly_core STATIC anomaly_core.cpp)
target_compile_features(anomaly_core PUBLIC cxx_std_20)
target_include_directories(anomaly_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(anomaly_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(anomaly_core PRIVATE ${PJ_WARNING_FLAGS})

# plugin_sdk (PlotMarker) + pj_scripting_core (the SeriesView/runMarkerScript types
# aliased in the header) are PUBLIC; nlohmann_json is a .cpp implementation detail.
target_link_libraries(anomaly_core
  PUBLIC
    plotjuggler_sdk::plugin_sdk
    pj_scripting_core::pj_scripting_core
  PRIVATE
    nlohmann_json::nlohmann_json
)
