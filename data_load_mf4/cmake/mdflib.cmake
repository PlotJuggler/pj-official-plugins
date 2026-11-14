# mdflib.cmake -- mdflib (ihedvall/mdflib) as a static library. Shared by this
# plugin and PlotJuggler's WebAssembly build: each fetches the same pinned source
# and calls pj_add_mdflib_library(<target> <mdflib source dir>). Needs ZLIB::ZLIB
# and EXPAT::EXPAT.
#
# mdflib is built from its globbed sources rather than add_subdirectory'd:
#   1. its CMakeLists uses ${CMAKE_SOURCE_DIR} for script/{zlib,expat}.cmake and
#      its include dir, which under add_subdirectory is the PARENT project.
#   2. it does not compile on gcc 15: its public headers idatawriter.h
#      (std::reverse) and sr4block.h (std::copy_n) rely on transitive
#      <algorithm>/<cstdint> that gcc 15 dropped, so they are force-included.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/VendoredPatch.cmake)

function(pj_add_mdflib_library target source_dir)
file(GLOB _mdf_sources CONFIGURE_DEPENDS ${source_dir}/mdflib/src/*.cpp)
# Emscripten's libc is musl, whose strerror_r is the XSI one (returns int).
# mdflib's non-Windows branch expects glibc's (returns char*), so route
# Emscripten through its Cygwin branch, which ignores the return value. Applied
# on every platform (a no-op elsewhere) so this repo's CI checks the anchor.
file(READ ${source_dir}/mdflib/src/platform.cpp _mdf_platform_cpp)
set(_mdf_strerror_anchor "#elif (__CYGWIN__)")
set(_mdf_strerror_fixed "#elif (__CYGWIN__ || __EMSCRIPTEN__)")
pj_apply_vendored_patch(_mdf_platform_cpp _mdf_strerror_anchor _mdf_strerror_fixed "data_load_mf4: mdflib musl strerror_r")
set(_mdf_patched_dir ${CMAKE_CURRENT_BINARY_DIR}/${target}_patched)
file(CONFIGURE OUTPUT ${_mdf_patched_dir}/platform.cpp CONTENT "${_mdf_platform_cpp}" @ONLY)
list(REMOVE_ITEM _mdf_sources ${source_dir}/mdflib/src/platform.cpp)
list(APPEND _mdf_sources ${_mdf_patched_dir}/platform.cpp)
add_library(${target} STATIC ${_mdf_sources})
set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(${target} PRIVATE cxx_std_17)
# SYSTEM so mdflib's headers don't trip the plugin's -Werror.
target_include_directories(${target} SYSTEM PUBLIC ${source_dir}/include)
target_include_directories(${target} PRIVATE ${source_dir}/mdflib/src)
target_link_libraries(${target} PRIVATE ZLIB::ZLIB EXPAT::EXPAT)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # gcc-15 transitive-include workaround (reason 2 above). PUBLIC: the gap is in
  # mdflib's public headers, so every consumer needs it too.
  target_compile_options(${target} PUBLIC "SHELL:-include algorithm" "SHELL:-include cstdint")
endif()
if(EMSCRIPTEN)
  # WebAssembly has no floating-point exceptions: Emscripten's <fenv.h> names no
  # FE_* flag. Turning off half.hpp's <cfenv> use makes it define its own; its
  # floating-point error handling, the only user of <cfenv>, is off anyway.
  target_compile_definitions(${target} PRIVATE HALF_ENABLE_CPP11_CFENV=0)
endif()
if(MSVC)
  target_compile_definitions(${target} PRIVATE _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS _CRT_SECURE_NO_WARNINGS)
endif()
endfunction()
