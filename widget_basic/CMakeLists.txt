# The one plugin family that links Qt. Everything else in this repository must not (the
# FORBID_QT gate in cmake/CheckElfPluginExports.cmake enforces it); this library is exempt because
# pj_harden_plugin_exports() sees PJ_get_widget_vtable among its REQUIRED_EXPORTS.
#
# Qt comes from the official installer, never from Conan or the distribution: the host refuses a
# widget library built against another major.minor. CI sets QT_ROOT_DIR (install-qt-action does);
# locally export QT_ROOT_DIR=<kit>/gcc_64 or put the kit on CMAKE_PREFIX_PATH.
find_package(nlohmann_json REQUIRED)
find_package(Qt6 6.12 REQUIRED COMPONENTS Widgets HINTS $ENV{QT_ROOT_DIR})

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)

add_library(basic_widgets_plugin SHARED
  basic_widgets.cpp
  canvas.cpp
  gauge_canvas.cpp
  numeric_indicator_canvas.cpp
  traffic_light_canvas.cpp
  value_table_canvas.cpp
  gauge.ui
  numeric_indicator.ui
  traffic_light.ui
  value_table.ui
)
set_target_properties(basic_widgets_plugin PROPERTIES
  AUTOMOC ON
  AUTOUIC ON
)
target_compile_features(basic_widgets_plugin PRIVATE cxx_std_20)
target_compile_options(basic_widgets_plugin PRIVATE ${PJ_WARNING_FLAGS})
# The promoted widgets in the .ui files are included by bare file name.
target_include_directories(basic_widgets_plugin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(basic_widgets_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  Qt6::Widgets
  nlohmann_json::nlohmann_json
)

pj_embed_manifest(basic_widgets_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/basic_widgets_manifest.hpp
  VAR_NAME kBasicWidgetsManifest
)

pj_emit_plugin_manifest(basic_widgets_plugin
  FAMILY        widget
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

pj_harden_plugin_exports(basic_widgets_plugin
  REQUIRED_EXPORTS PJ_get_widget_vtable)

# The library is loaded through PJ::WidgetLibrary, exactly as the host does, with a real
# QApplication on the offscreen platform.
if(TARGET GTest::gtest AND BUILD_TESTING)
  add_executable(basic_widgets_test tests/basic_widgets_test.cpp)
  target_compile_features(basic_widgets_test PRIVATE cxx_std_20)
  target_compile_options(basic_widgets_test PRIVATE ${PJ_WARNING_FLAGS})
  target_compile_definitions(basic_widgets_test PRIVATE
    PJ_BASIC_WIDGETS_PLUGIN_PATH="$<TARGET_FILE:basic_widgets_plugin>"
  )
  add_dependencies(basic_widgets_test basic_widgets_plugin)
  target_link_libraries(basic_widgets_test PRIVATE
    plotjuggler_sdk::plugin_host
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json
    Qt6::Widgets
    GTest::gtest
  )
  add_test(NAME basic_widgets_test COMMAND basic_widgets_test)
  # The paint comparisons need text to be drawn: the official Qt kit ships no fonts, and on Windows
  # the offscreen platform does not look in the system font directory by itself.
  set(_basic_widgets_test_env "QT_QPA_PLATFORM=offscreen")
  if(WIN32)
    list(APPEND _basic_widgets_test_env "QT_QPA_FONTDIR=C:/Windows/Fonts")
  endif()
  set_tests_properties(basic_widgets_test PROPERTIES ENVIRONMENT "${_basic_widgets_test_env}")
endif()
