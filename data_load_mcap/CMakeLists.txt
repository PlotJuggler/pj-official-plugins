find_package(nlohmann_json REQUIRED)
find_package(lz4 REQUIRED)
find_package(zstd REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# --- Plugin shared library ---
# mcap is header-only and vendored under contrib/mcap/ from the
# parallel-reader fork. lz4 + zstd come from conan for chunk decompression.
add_library(mcap_source_plugin SHARED mcap_source.cpp)
target_compile_features(mcap_source_plugin PRIVATE cxx_std_20)
target_compile_options(mcap_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(mcap_source_plugin SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
target_link_libraries(mcap_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json LZ4::lz4_static zstd::libzstd_static
)

pj_embed_ui(mcap_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/dialog_mcap.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/dialog_mcap_ui.hpp
  VAR_NAME kDialogMcapUi
)

pj_embed_manifest(mcap_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/mcap_manifest.hpp
  VAR_NAME kMcapManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(mcap_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Unit test for MCAP helpers ---
add_executable(mcap_helpers_test tests/mcap_helpers_test.cpp)
target_compile_features(mcap_helpers_test PRIVATE cxx_std_20)
target_compile_options(mcap_helpers_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(mcap_helpers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mcap_helpers_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
target_link_libraries(mcap_helpers_test PRIVATE LZ4::lz4_static zstd::libzstd_static GTest::gtest_main)
add_test(NAME mcap_helpers_test COMMAND mcap_helpers_test)

# --- Unit test for the MessageByteStore (hot/cold lazy byte layer) ---
find_package(Threads REQUIRED)
add_executable(message_byte_store_test tests/message_byte_store_test.cpp)
target_compile_features(message_byte_store_test PRIVATE cxx_std_20)
target_compile_options(message_byte_store_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(message_byte_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(message_byte_store_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
target_link_libraries(message_byte_store_test PRIVATE
  LZ4::lz4_static zstd::libzstd_static GTest::gtest_main Threads::Threads
)
add_test(NAME message_byte_store_test COMMAND message_byte_store_test)
