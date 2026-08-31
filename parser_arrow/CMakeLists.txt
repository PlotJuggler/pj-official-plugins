find_package(nanoarrow REQUIRED)
find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# nanoarrow/0.7.0's Conan component omits its bundled FlatCC runtime from
# nanoarrow::nanoarrow_ipc. Resolve that archive from the same package so IPC
# decoder symbols link on every consumer of parser_arrow_core.
set(_nanoarrow_library_dirs
  ${nanoarrow_LIB_DIRS_DEBUG}
  ${nanoarrow_LIB_DIRS_RELEASE}
  ${nanoarrow_LIB_DIRS_RELWITHDEBINFO}
  ${nanoarrow_LIB_DIRS_MINSIZEREL}
)
find_library(PARSER_ARROW_FLATCCRT_LIBRARY
  NAMES flatccrt
  HINTS ${_nanoarrow_library_dirs}
  REQUIRED
)

# --- Arrow IPC decoder and table shaper shared by the plugin and unit tests ---
add_library(parser_arrow_core STATIC
  src/ipc_decoder.cpp
  src/table_shaper.cpp
)
set_target_properties(parser_arrow_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(parser_arrow_core PUBLIC cxx_std_20)
target_compile_options(parser_arrow_core PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(parser_arrow_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(parser_arrow_core PUBLIC
  plotjuggler_sdk::base
  nanoarrow::nanoarrow
  nanoarrow::nanoarrow_ipc
  nlohmann_json::nlohmann_json
  ${PARSER_ARROW_FLATCCRT_LIBRARY}
)

# --- Plugin shared library ---
add_library(parser_arrow_plugin SHARED src/arrow_parser.cpp)
target_compile_features(parser_arrow_plugin PRIVATE cxx_std_20)
target_compile_options(parser_arrow_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_arrow_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  parser_arrow_core
  nlohmann_json::nlohmann_json
)

pj_embed_manifest(parser_arrow_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/arrow_manifest.hpp
  VAR_NAME kArrowManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(parser_arrow_plugin
  FAMILY        message_parser
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Integration test (loads plugin .so via host library) ---
add_executable(arrow_parser_test tests/arrow_parser_test.cpp)
target_compile_features(arrow_parser_test PRIVATE cxx_std_20)
target_compile_options(arrow_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(arrow_parser_test PRIVATE
  PJ_ARROW_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_arrow_plugin>"
  PJ_ARROW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data"
)
target_include_directories(arrow_parser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(arrow_parser_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  plotjuggler_sdk::plugin_host
  nanoarrow::nanoarrow
  GTest::gtest_main
)
add_test(NAME arrow_parser_test COMMAND arrow_parser_test)

# --- Unit test for the Arrow IPC decoder ---
add_executable(ipc_decoder_test tests/ipc_decoder_test.cpp)
target_compile_features(ipc_decoder_test PRIVATE cxx_std_20)
target_compile_options(ipc_decoder_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(ipc_decoder_test PRIVATE
  PJ_ARROW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data"
)
target_include_directories(ipc_decoder_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_link_libraries(ipc_decoder_test PRIVATE
  parser_arrow_core
  GTest::gtest_main
)
add_test(NAME ipc_decoder_test COMMAND ipc_decoder_test)

# --- Unit test for decoded Arrow stream shaping ---
add_executable(table_shaper_test tests/table_shaper_test.cpp)
target_compile_features(table_shaper_test PRIVATE cxx_std_20)
target_compile_options(table_shaper_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(table_shaper_test PRIVATE
  PJ_ARROW_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data"
)
target_include_directories(table_shaper_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_link_libraries(table_shaper_test PRIVATE
  parser_arrow_core
  GTest::gtest_main
)
add_test(NAME table_shaper_test COMMAND table_shaper_test)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(parser_arrow_plugin
  REQUIRED_EXPORTS PJ_get_message_parser_vtable)
