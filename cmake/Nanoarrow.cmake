# nanoarrow::nanoarrow (Apache-2.0, C), 0.9.0, linked statically into the plugin.
#  - An installed 0.9.0 wins: Conan once ConanCenter ships 0.9 (conan-center-index#30738),
#    or a distro/conda package. Its config demands an exact version match.
#  - Otherwise the pinned Apache source release is built here (core library only: no IPC,
#    so no flatcc); -DFETCHCONTENT_SOURCE_DIR_NANOARROW=<unpacked tarball> builds offline.
# Drop the fetch when ConanCenter ships nanoarrow 0.9: put "nanoarrow/0.9.x" back in the
# conanfile.py requires (root + parser_arrow) and keep only the find_package.
include_guard(GLOBAL)

find_package(nanoarrow 0.9.0 CONFIG QUIET)
if(NOT nanoarrow_FOUND)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  include(FetchContent)
  FetchContent_Declare(nanoarrow
    URL https://github.com/apache/arrow-nanoarrow/releases/download/apache-arrow-nanoarrow-0.9.0/apache-arrow-nanoarrow-0.9.0.tar.gz
    URL_HASH SHA256=801200a0e95e869d5c4bdeb5b535dba58551482bb782b7dc8bd599c8b6e8cacf)
  FetchContent_MakeAvailable(nanoarrow)
  # Build only what is linked: upstream also defines a shared variant, which would land in
  # bin/ beside the plugins. Its headers are system headers, out of PJ_WARNING_FLAGS' reach.
  set_property(DIRECTORY "${nanoarrow_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL ON)
  get_target_property(_pj_nanoarrow_includes nanoarrow_static INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(nanoarrow_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_pj_nanoarrow_includes}")
endif()

# Plugins are self-contained: a shared nanoarrow would become a runtime dependency.
get_target_property(_pj_nanoarrow_type nanoarrow::nanoarrow TYPE)
if(_pj_nanoarrow_type STREQUAL "SHARED_LIBRARY")
  message(FATAL_ERROR "nanoarrow::nanoarrow is a shared library (${nanoarrow_DIR}); plugins link it statically. "
                      "Configure with -DCMAKE_DISABLE_FIND_PACKAGE_nanoarrow=ON to build the pinned source.")
endif()
