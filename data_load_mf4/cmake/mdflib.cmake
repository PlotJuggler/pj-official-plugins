# mdflib.cmake -- mdflib (ihedvall/mdflib) as a static library. Shared by this
# plugin and PlotJuggler's WebAssembly build: each fetches the same pinned source
# and calls pj_add_mdflib_library(<target> <mdflib source dir>). Needs ZLIB::ZLIB
# and EXPAT::EXPAT.
include_guard(GLOBAL)

function(pj_add_mdflib_library target source_dir)
file(GLOB _mdf_sources CONFIGURE_DEPENDS ${source_dir}/mdflib/src/*.cpp)
add_library(${target} STATIC ${_mdf_sources})
set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(${target} PRIVATE cxx_std_17)
# SYSTEM so mdflib's headers don't trip the plugin's -Werror.
target_include_directories(${target} SYSTEM PUBLIC ${source_dir}/include)
target_include_directories(${target} PRIVATE ${source_dir}/mdflib/src)
target_link_libraries(${target} PRIVATE ZLIB::ZLIB EXPAT::EXPAT)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # gcc-15 transitive-include workaround (see data_load_mf4/CMakeLists.txt).
  target_compile_options(${target} PRIVATE "SHELL:-include algorithm" "SHELL:-include cstdint")
endif()
if(MSVC)
  target_compile_definitions(${target} PRIVATE _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS _CRT_SECURE_NO_WARNINGS)
endif()
endfunction()
