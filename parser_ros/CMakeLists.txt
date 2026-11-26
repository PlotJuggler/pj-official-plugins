find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# Keeps rosx_introspection from calling ament_package() when ROS is installed on the
# host but this is not a colcon workspace.
set(CMAKE_DISABLE_FIND_PACKAGE_ament_cmake TRUE)

# 3.1.3: IDL-support APIs, quoted text in IDL annotation arguments (#48), null/truncated
# CDR headers (#49).
CPMAddPackage(
  NAME rosx_introspection
  URL https://github.com/facontidavide/rosx_introspection/archive/refs/tags/3.1.3.zip
  URL_HASH SHA256=d12637fc579164c302067f0f6f6beca0c5739a89983c91317eb3f7be5d67a599
  OPTIONS "ROSX_HAS_JSON OFF"
)

# PIC: it is statically linked into the plugin .so.
set_target_properties(rosx_introspection PROPERTIES POSITION_INDEPENDENT_CODE ON)

# rosx_introspection bundles peglib.h, which requires __cplusplus >= 201703L; MSVC
# reports 199711L without /Zc:__cplusplus (C1189 "Requires complete C++17 support").
target_compile_options(rosx_introspection PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>)

# SYSTEM include dirs, so -Werror does not fire on the third-party headers.
get_target_property(_rosx_includes rosx_introspection INTERFACE_INCLUDE_DIRECTORIES)
if(_rosx_includes)
  set_target_properties(rosx_introspection PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_rosx_includes}")
endif()

# Header-only DataTamer parsing library. parser_data_tamer declares the same CPM package:
# keep the two in step.
CPMAddPackage(
  NAME data_tamer_2
  URL https://github.com/facontidavide/data_tamer/archive/refs/tags/2.0.0.zip
  URL_HASH SHA256=0a8caef329d641b2cc72fa68211af37a36759614807add4bdba2ed2bf2fc8963
  DOWNLOAD_ONLY YES
)
add_library(data_tamer_parser INTERFACE)
target_include_directories(data_tamer_parser SYSTEM INTERFACE
  "${data_tamer_2_SOURCE_DIR}/data_tamer_cpp/include")

set(PARSER_ROS_COMMON_SOURCES
  ros_parser.cpp
  ros_specializations.cpp
  ros_builtin_object_handlers.cpp
  ros_marker_handlers.cpp
  ros_yolo_handlers.cpp
)

function(configure_parser_ros_target TARGET)
  target_compile_features(${TARGET} PRIVATE cxx_std_20)
  target_compile_options(${TARGET} PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(${TARGET} PRIVATE
    plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json rosx_introspection data_tamer_parser
    pj_laser_scan pj_grid_map pj_pointcloud_color)
endfunction()

add_library(parser_ros_plugin SHARED
  ${PARSER_ROS_COMMON_SOURCES}
  ros_parser_plugin.cpp
)
configure_parser_ros_target(parser_ros_plugin)

pj_embed_manifest(parser_ros_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/ros_manifest.hpp
  VAR_NAME kRosManifest
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

pj_embed_ui(parser_ros_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ros_parser_options.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/ros_parser_options_ui.hpp
  VAR_NAME kRosParserOptionsUi
)

# Sidecar .pjmanifest.json the host scans before dlopen.
pj_emit_plugin_manifest(parser_ros_plugin
  FAMILY        message_parser
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# The object route returns sdk::BuiltinObject (std::any) through the in-process C++ plugin
# pointer. On macOS, hidden RTTI in the dylib gives libc++ distinct type identities for the
# builtin structs, so std::any_cast in the host/test executable fails on a correct object.
# Darwin therefore exports C++ RTTI for this plugin; ELF keeps the isolation defaults.
if(APPLE)
  set_target_properties(parser_ros_plugin PROPERTIES
    CXX_VISIBILITY_PRESET default
    C_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
  )
endif()

add_executable(ros_parser_test tests/ros_parser_test.cpp)
target_compile_features(ros_parser_test PRIVATE cxx_std_20)
target_compile_options(ros_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(ros_parser_test PRIVATE
  PJ_ROS_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_ros_plugin>"
  PJ_ROS_PARSER_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
target_link_libraries(ros_parser_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  plotjuggler_sdk::plugin_host
  rosx_introspection
  nlohmann_json::nlohmann_json
  GTest::gtest_main
)
add_test(NAME ros_parser_test COMMAND ros_parser_test)

add_executable(bound_field_cache_test tests/bound_field_cache_test.cpp)
target_compile_features(bound_field_cache_test PRIVATE cxx_std_20)
target_compile_options(bound_field_cache_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(bound_field_cache_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bound_field_cache_test PRIVATE plotjuggler_sdk::plugin_sdk GTest::gtest_main)
add_test(NAME bound_field_cache_test COMMAND bound_field_cache_test)

find_package(benchmark QUIET)
if(benchmark_FOUND)
  add_executable(ros_parser_bench benchmarks/ros_parser_bench.cpp)
  target_compile_features(ros_parser_bench PRIVATE cxx_std_20)
  target_compile_definitions(ros_parser_bench PRIVATE
    PJ_ROS_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_ros_plugin>"
  )
  target_link_libraries(ros_parser_bench PRIVATE
    plotjuggler_sdk::plugin_sdk plotjuggler_sdk::plugin_host rosx_introspection benchmark::benchmark)
endif()

# Opt-in dev tool, never built by CI: mcap_decode_bench [--read-only] [--hash] file.mcap...
# The vendored mcap reader needs lz4 + zstd. The root conanfile pins both (the aggregate
# build has them); a per-plugin build installs them next to its own deps, e.g.
# `conan install --requires=lz4/1.9.4 --requires=zstd/1.5.7 -g CMakeDeps --output-folder=<dir>`
# plus <dir> in CMAKE_PREFIX_PATH. System libraries are not used: Ubuntu 22.04's lz4 is
# older than the reader needs.
option(PARSER_ROS_BUILD_DECODE_BENCH "Build mcap_decode_bench (needs the Conan lz4 and zstd packages)" OFF)
if(PARSER_ROS_BUILD_DECODE_BENCH)
  find_package(lz4 QUIET)
  find_package(zstd QUIET)
  if(NOT TARGET LZ4::lz4_static OR NOT TARGET zstd::libzstd_static)
    message(FATAL_ERROR
      "PARSER_ROS_BUILD_DECODE_BENCH needs the Conan lz4 and zstd packages (targets LZ4::lz4_static and "
      "zstd::libzstd_static). Build the aggregate (root conanfile), or run `conan install --requires=lz4/1.9.4 "
      "--requires=zstd/1.5.7 -g CMakeDeps --output-folder=<dir>` and add <dir> to CMAKE_PREFIX_PATH.")
  endif()
  add_executable(mcap_decode_bench benchmarks/mcap_decode_bench.cpp)
  target_compile_features(mcap_decode_bench PRIVATE cxx_std_20)
  target_compile_definitions(mcap_decode_bench PRIVATE
    PJ_ROS_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_ros_plugin>")
  target_include_directories(mcap_decode_bench SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../data_load_mcap/contrib)
  target_link_libraries(mcap_decode_bench PRIVATE
    plotjuggler_sdk::plugin_sdk plotjuggler_sdk::plugin_host LZ4::lz4_static zstd::libzstd_static)
  add_dependencies(mcap_decode_bench parser_ros_plugin)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(parser_ros_plugin
  REQUIRED_EXPORTS PJ_get_message_parser_vtable PJ_get_dialog_vtable)
