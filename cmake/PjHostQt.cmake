# PjHostQt.cmake
#
# pj_find_host_qt(COMPONENTS <Qt6 component>...)
#
# Finds the official Qt kit whose major.minor the host was built with, for the plugins that link Qt
# (widget_basic, data_stream_can). The host's Qt is pinned in one place, the QT_VERSION file at the
# repository root, which the CI workflows also read to install that kit. Qt comes from the official
# installer, never from Conan or the distribution: the host refuses a widget library built against
# another major.minor, and has no such check for other families, which would fail at dlopen.
# CI sets QT_ROOT_DIR (install-qt-action does); locally export QT_ROOT_DIR=<kit>/gcc_64 or put the
# kit on CMAKE_PREFIX_PATH.
#
# Fails the configure when the kit found is another major.minor (find_package accepts any newer
# minor of the same major; the host does not) or when the compiler is not GCC (Linux) / MSVC
# (Windows), the host's own compiler families.
#
# A macro, not a function: PJ_HOST_QT_MAJOR / PJ_HOST_QT_MINOR (and the imported Qt6:: targets'
# variables) stay set in the caller's scope, where the widget test target reads them.
set(_PJ_HOST_QT_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../QT_VERSION")  # resolved at include time

macro(pj_find_host_qt)
  set(_pj_qt_version_file "${_PJ_HOST_QT_VERSION_FILE}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pj_qt_version_file}")
  file(READ "${_pj_qt_version_file}" _pj_qt_version)
  string(STRIP "${_pj_qt_version}" _pj_qt_version)
  if(NOT _pj_qt_version MATCHES "^([0-9]+)\\.([0-9]+)\\.[0-9]+$")
    message(FATAL_ERROR "QT_VERSION must hold an exact version such as 6.12.0; found \"${_pj_qt_version}\"")
  endif()
  set(PJ_HOST_QT_MAJOR ${CMAKE_MATCH_1})
  set(PJ_HOST_QT_MINOR ${CMAKE_MATCH_2})

  find_package(Qt6 ${PJ_HOST_QT_MAJOR}.${PJ_HOST_QT_MINOR} REQUIRED ${ARGN} HINTS $ENV{QT_ROOT_DIR})

  if(NOT Qt6_VERSION MATCHES "^${PJ_HOST_QT_MAJOR}\\.${PJ_HOST_QT_MINOR}\\.")
    message(FATAL_ERROR
      "Plugins linking Qt must be built with Qt ${PJ_HOST_QT_MAJOR}.${PJ_HOST_QT_MINOR}.x, the host's Qt; "
      "found ${Qt6_VERSION} (${Qt6_DIR})")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR
      "Plugins linking Qt must be built with GCC (Linux) or MSVC (Windows); found ${CMAKE_CXX_COMPILER_ID}")
  endif()
endmacro()
