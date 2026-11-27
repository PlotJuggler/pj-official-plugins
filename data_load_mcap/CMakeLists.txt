find_package(nlohmann_json REQUIRED)
find_package(lz4 REQUIRED)
find_package(zstd REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# mcap is header-only and vendored under contrib/mcap/ (parallel-reader fork); lz4 + zstd decompress chunks.
add_library(mcap_source_plugin SHARED mcap_source.cpp)
target_compile_features(mcap_source_plugin PRIVATE cxx_std_20)
target_compile_options(mcap_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(mcap_source_plugin SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
target_link_libraries(mcap_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json LZ4::lz4_static zstd::libzstd_static pj_always_included
  pj_utf8_path
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

# Sidecar .pjmanifest.json (manifest.json + abi_major + family) the host scans before dlopen.
pj_emit_plugin_manifest(mcap_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

add_executable(mcap_helpers_test tests/mcap_helpers_test.cpp)
target_compile_features(mcap_helpers_test PRIVATE cxx_std_20)
target_compile_options(mcap_helpers_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(mcap_helpers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mcap_helpers_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
target_link_libraries(mcap_helpers_test PRIVATE LZ4::lz4_static zstd::libzstd_static pj_utf8_path GTest::gtest_main)
add_test(NAME mcap_helpers_test COMMAND mcap_helpers_test)

add_executable(mcap_message_decompression_test tests/mcap_message_decompression_test.cpp)
target_compile_features(mcap_message_decompression_test PRIVATE cxx_std_20)
target_compile_options(mcap_message_decompression_test PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(mcap_message_decompression_test PRIVATE plotjuggler_sdk::base zstd::libzstd_static GTest::gtest_main)
add_test(NAME mcap_message_decompression_test COMMAND mcap_message_decompression_test)

add_executable(mcap_dialog_test tests/mcap_dialog_test.cpp)
target_compile_features(mcap_dialog_test PRIVATE cxx_std_20)
target_compile_options(mcap_dialog_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(mcap_dialog_test PRIVATE
  MCAP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data"
)
target_include_directories(mcap_dialog_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_include_directories(mcap_dialog_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
target_link_libraries(mcap_dialog_test PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json LZ4::lz4_static zstd::libzstd_static GTest::gtest_main
  pj_always_included pj_utf8_path
)
add_test(NAME mcap_dialog_test COMMAND mcap_dialog_test)

add_executable(mcap_dataset_metadata_test tests/mcap_dataset_metadata_test.cpp)
target_compile_features(mcap_dataset_metadata_test PRIVATE cxx_std_20)
target_compile_options(mcap_dataset_metadata_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(mcap_dataset_metadata_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mcap_dataset_metadata_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
target_link_libraries(mcap_dataset_metadata_test PRIVATE
  plotjuggler_sdk::base nlohmann_json::nlohmann_json LZ4::lz4_static zstd::libzstd_static GTest::gtest_main
)
add_test(NAME mcap_dataset_metadata_test COMMAND mcap_dataset_metadata_test)

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

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(mcap_source_plugin
  REQUIRED_EXPORTS PJ_get_data_source_vtable PJ_get_dialog_vtable)

add_executable(mcap_source_split_bag_test tests/mcap_source_split_bag_test.cpp)
target_compile_features(mcap_source_split_bag_test PRIVATE cxx_std_20)
target_compile_options(mcap_source_split_bag_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(mcap_source_split_bag_test PRIVATE
  MCAP_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mcap_source_plugin>"
)
target_include_directories(mcap_source_split_bag_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
target_link_libraries(mcap_source_split_bag_test PRIVATE
  plotjuggler_sdk::plugin_host LZ4::lz4_static zstd::libzstd_static pj_utf8_path GTest::gtest_main
)
add_dependencies(mcap_source_split_bag_test mcap_source_plugin)
add_test(NAME mcap_source_split_bag_test COMMAND mcap_source_split_bag_test)

add_executable(mcap_source_message_compression_test tests/mcap_source_message_compression_test.cpp)
target_compile_features(mcap_source_message_compression_test PRIVATE cxx_std_20)
target_compile_options(mcap_source_message_compression_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(mcap_source_message_compression_test PRIVATE
  MCAP_SOURCE_PLUGIN_PATH="$<TARGET_FILE:mcap_source_plugin>"
  MCAP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_data"
)
target_include_directories(mcap_source_message_compression_test SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
target_link_libraries(mcap_source_message_compression_test PRIVATE
  plotjuggler_sdk::plugin_host LZ4::lz4_static zstd::libzstd_static pj_utf8_path GTest::gtest_main
)
add_dependencies(mcap_source_message_compression_test mcap_source_plugin)
add_test(NAME mcap_source_message_compression_test COMMAND mcap_source_message_compression_test)
