find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

CPMAddPackage(
  NAME ulog_cpp
  URL https://github.com/PX4/ulog_cpp/archive/refs/tags/v1.0.1.zip
)

# Ensure ulog_cpp is statically linked into the plugin .so.
set_target_properties(ulog_cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Mark ulog_cpp include dirs as SYSTEM to suppress -Werror on
# third-party headers.
get_target_property(_ulog_includes ulog_cpp INTERFACE_INCLUDE_DIRECTORIES)
if(_ulog_includes)
  set_target_properties(ulog_cpp PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_ulog_includes}")
endif()

# --- Plugin shared library ---
add_library(ulog_source_plugin SHARED ulog_source.cpp ulog_params_dialog.cpp ulog_container.cpp)
target_compile_features(ulog_source_plugin PRIVATE cxx_std_20)
target_compile_options(ulog_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
if(MSVC)
  # Suppress C4702 (unreachable code) from ulog_cpp third-party headers.
  target_compile_options(ulog_source_plugin PRIVATE /wd4702)
endif()
target_link_libraries(ulog_source_plugin PRIVATE plotjuggler_sdk::plugin_sdk ulog_cpp::ulog_cpp nlohmann_json::nlohmann_json)

pj_embed_manifest(ulog_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/ulog_manifest.hpp
  VAR_NAME kUlogManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(ulog_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

pj_embed_ui(ulog_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/ulog_params.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/ulog_params_ui.hpp
  VAR_NAME kULogParamsUi
)

# --- Integration test ---
add_executable(ulog_source_test tests/ulog_source_test.cpp ulog_container.cpp)
target_compile_features(ulog_source_test PRIVATE cxx_std_20)
target_compile_options(ulog_source_test PRIVATE ${PJ_WARNING_FLAGS})
if(MSVC)
  target_compile_options(ulog_source_test PRIVATE /wd4702)
endif()
target_compile_definitions(ulog_source_test PRIVATE
  ULOG_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data"
  ULOG_SOURCE_PLUGIN_PATH="$<TARGET_FILE:ulog_source_plugin>"
)
# plugin_host: the import tests load the built plugin and drive it end to end.
target_link_libraries(ulog_source_test PRIVATE
  ulog_cpp::ulog_cpp plotjuggler_sdk::plugin_host GTest::gtest_main)
add_dependencies(ulog_source_test ulog_source_plugin)
add_test(NAME ulog_source_test COMMAND ulog_source_test)

# --- Import benchmark (not a test: run it by hand, see README) ---
add_executable(ulog_import_bench benchmarks/ulog_import_bench.cpp)
target_compile_features(ulog_import_bench PRIVATE cxx_std_20)
target_compile_options(ulog_import_bench PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(ulog_import_bench PRIVATE
  ULOG_SOURCE_PLUGIN_PATH="$<TARGET_FILE:ulog_source_plugin>"
  ULOG_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data"
)
target_link_libraries(ulog_import_bench PRIVATE plotjuggler_sdk::plugin_host)
add_dependencies(ulog_import_bench ulog_source_plugin)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(ulog_source_plugin
  REQUIRED_EXPORTS PJ_get_data_source_vtable PJ_get_dialog_vtable)
