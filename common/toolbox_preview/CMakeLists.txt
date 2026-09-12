# Shared "preview a plot + run a generator" engine for generator toolboxes
# (toolbox_anomaly_detector now; toolbox_transform_editor during the convergence PR).
# Internal target — not exported, not installed; consumers in this repo link
# pj_toolbox_preview directly.
#
# UI-free: catalog cache + streaming refresh, source-curve downsampling, and the change-gated
# ephemeral generator submit/read-back. The only generator-specific part (which host service,
# and how the OUTPUT renders — markers vs a derived series) is a per-plugin PreviewBackend.

find_package(nlohmann_json REQUIRED)

add_library(pj_toolbox_preview STATIC
  series_catalog.cpp
  series_types.cpp
  ephemeral_preview.cpp
)
target_compile_features(pj_toolbox_preview PUBLIC cxx_std_20)
if(DEFINED PJ_WARNING_FLAGS)
  target_compile_options(pj_toolbox_preview PRIVATE ${PJ_WARNING_FLAGS})
endif()

# Static lib goes inside SHARED plugin .so files → must be position-independent.
set_target_properties(pj_toolbox_preview PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(pj_toolbox_preview PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# SDK types appear across the public API (ChartSeries/ChartMarker, ToolboxHostView,
# PlotMarkers); nlohmann/json arrives transitively via widget_data.hpp.
target_link_libraries(pj_toolbox_preview PUBLIC
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
)

# Unit tests for the cadence policy and the change-gated read-back (mirrors the other
# common/ libs' test wiring).
if(TARGET GTest::gtest_main)
  add_executable(toolbox_preview_test
    tests/adaptive_poll_test.cpp
    tests/ephemeral_preview_test.cpp
    tests/series_decode_test.cpp
    tests/series_catalog_test.cpp
  )
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(toolbox_preview_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(toolbox_preview_test PRIVATE
    pj_toolbox_preview
    GTest::gtest_main
  )
  add_test(NAME toolbox_preview_test COMMAND toolbox_preview_test)
endif()
