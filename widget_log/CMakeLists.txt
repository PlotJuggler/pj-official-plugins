# A Widget-family library: it links Qt, which every non-widget plugin of this repository must not (the
# FORBID_QT gate in cmake/CheckElfPluginExports.cmake enforces it); this library is exempt because
# pj_harden_plugin_exports() sees PJ_get_widget_vtable among its REQUIRED_EXPORTS.
#
# Qt comes from the official installer, never from Conan or the distribution: the host refuses a
# widget library built against another major.minor. CI sets QT_ROOT_DIR (install-qt-action does);
# locally export QT_ROOT_DIR=<kit>/gcc_64 or put the kit on CMAKE_PREFIX_PATH.
#
# The host's Qt is pinned in one place, the QT_VERSION file at the repository root, which the CI
# workflows also read to install that kit.
set(_pj_qt_version_file "${CMAKE_CURRENT_LIST_DIR}/../QT_VERSION")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pj_qt_version_file}")
file(READ "${_pj_qt_version_file}" _pj_qt_version)
string(STRIP "${_pj_qt_version}" _pj_qt_version)
if(NOT _pj_qt_version MATCHES "^([0-9]+)\\.([0-9]+)\\.[0-9]+$")
  message(FATAL_ERROR "QT_VERSION must hold an exact version such as 6.12.0; found \"${_pj_qt_version}\"")
endif()
set(PJ_HOST_QT_MAJOR ${CMAKE_MATCH_1})
set(PJ_HOST_QT_MINOR ${CMAKE_MATCH_2})

find_package(nlohmann_json REQUIRED)
find_package(Qt6 ${PJ_HOST_QT_MAJOR}.${PJ_HOST_QT_MINOR} REQUIRED COMPONENTS Widgets HINTS $ENV{QT_ROOT_DIR})

# find_package accepts any newer minor of the same major; the host does not.
if(NOT Qt6_VERSION MATCHES "^${PJ_HOST_QT_MAJOR}\\.${PJ_HOST_QT_MINOR}\\.")
  message(FATAL_ERROR
    "Widget plugins must be built with Qt ${PJ_HOST_QT_MAJOR}.${PJ_HOST_QT_MINOR}.x, the host's Qt; "
    "found ${Qt6_VERSION} (${Qt6_DIR})")
endif()
# The host checks the compiler family of a widget library: its Linux build is GCC, its Windows build MSVC.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  message(FATAL_ERROR
    "Widget plugins must be built with GCC (Linux) or MSVC (Windows); found ${CMAKE_CXX_COMPILER_ID}")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)

# Store, loader and filter: Qt-free, so the tests exercise them without a host or a QApplication.
add_library(log_viewer_core STATIC
  log_filter.cpp
  log_loader.cpp
  log_store.cpp
)
set_target_properties(log_viewer_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(log_viewer_core PUBLIC cxx_std_20)
target_compile_options(log_viewer_core PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(log_viewer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(log_viewer_core PUBLIC plotjuggler_sdk::plugin_sdk)

add_library(log_viewer_plugin SHARED
  log_viewer_plugin.cpp
  log_viewer_widget.cpp
  log_table_model.cpp
  log_viewer.ui
)
set_target_properties(log_viewer_plugin PROPERTIES
  AUTOMOC ON
  AUTOUIC ON
)
target_compile_features(log_viewer_plugin PRIVATE cxx_std_20)
target_compile_options(log_viewer_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(log_viewer_plugin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(log_viewer_plugin PRIVATE
  log_viewer_core
  plotjuggler_sdk::plugin_sdk
  Qt6::Widgets
  nlohmann_json::nlohmann_json
)

pj_embed_manifest(log_viewer_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/log_viewer_manifest.hpp
  VAR_NAME kLogViewerManifest
)

pj_emit_plugin_manifest(log_viewer_plugin
  FAMILY        widget
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

pj_harden_plugin_exports(log_viewer_plugin
  REQUIRED_EXPORTS PJ_get_widget_vtable)

if(TARGET GTest::gtest AND BUILD_TESTING)
  # The paint and font metrics need fonts: the official Qt kit ships none, and on Windows the
  # offscreen platform does not look in the system font directory by itself.
  set(_log_viewer_test_env "QT_QPA_PLATFORM=offscreen")
  if(WIN32)
    list(APPEND _log_viewer_test_env "QT_QPA_FONTDIR=C:/Windows/Fonts")
  endif()

  # Store, loader (against a fake widget host) and filters.
  add_executable(log_viewer_core_test tests/log_core_test.cpp)
  target_compile_features(log_viewer_core_test PRIVATE cxx_std_20)
  target_compile_options(log_viewer_core_test PRIVATE ${PJ_WARNING_FLAGS})
  target_include_directories(log_viewer_core_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(log_viewer_core_test PRIVATE
    log_viewer_core
    plotjuggler_sdk::plugin_host
    plotjuggler_sdk::plugin_sdk
    GTest::gtest
    GTest::gtest_main
  )
  add_test(NAME log_viewer_core_test COMMAND log_viewer_core_test)

  # The library loaded through PJ::WidgetLibrary, exactly as the host does, with a real QApplication on
  # the offscreen platform.
  find_package(Qt6 ${PJ_HOST_QT_MAJOR}.${PJ_HOST_QT_MINOR} REQUIRED COMPONENTS Test HINTS $ENV{QT_ROOT_DIR})
  add_executable(log_viewer_test tests/log_viewer_test.cpp)
  target_compile_features(log_viewer_test PRIVATE cxx_std_20)
  target_compile_options(log_viewer_test PRIVATE ${PJ_WARNING_FLAGS})
  target_compile_definitions(log_viewer_test PRIVATE
    PJ_LOG_VIEWER_PLUGIN_PATH="$<TARGET_FILE:log_viewer_plugin>"
    PJ_EXPECTED_QT_MAJOR=${PJ_HOST_QT_MAJOR}
    PJ_EXPECTED_QT_MINOR=${PJ_HOST_QT_MINOR}
  )
  target_include_directories(log_viewer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_dependencies(log_viewer_test log_viewer_plugin)
  target_link_libraries(log_viewer_test PRIVATE
    plotjuggler_sdk::plugin_host
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json
    Qt6::Widgets
    Qt6::Test
    GTest::gtest
  )
  add_test(NAME log_viewer_test COMMAND log_viewer_test)
  set_tests_properties(log_viewer_test PROPERTIES ENVIRONMENT "${_log_viewer_test_env}")
endif()
