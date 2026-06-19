# anomaly_core — GUI-free detection engine shared by the toolbox plugin and the
# headless CLI runner. Static lib (PIC, so it can link into the SHARED plugin).

find_package(lua REQUIRED)
find_package(sol2 REQUIRED)
find_package(kissfft REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(anomaly_core STATIC anomaly_core.cpp)
target_compile_features(anomaly_core PUBLIC cxx_std_20)
target_include_directories(anomaly_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(anomaly_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# -Wno-array-bounds: GCC false-positive deep in sol2's stack_field.hpp when reading
# string-keyed tables (the opts table). Optimizer diagnostic in a third-party header.
target_compile_options(anomaly_core PRIVATE ${PJ_WARNING_FLAGS} -Wno-array-bounds)

# plugin_sdk + sol2 are PUBLIC (exposed via the header's PlotMarker / engine API);
# kissfft + nlohmann_json stay PRIVATE (implementation detail of the .cpp).
target_link_libraries(anomaly_core
  PUBLIC
    plotjuggler_sdk::plugin_sdk
    lua::lua
    sol2::sol2
  PRIVATE
    kissfft::kissfft
    nlohmann_json::nlohmann_json
)
