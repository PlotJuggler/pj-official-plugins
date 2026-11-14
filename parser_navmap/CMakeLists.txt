find_package(nlohmann_json REQUIRED)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# Decoder + mesh conversion, shared by the plugin and the unit tests.
add_library(parser_navmap_core STATIC
  navmap_msg_decoder.cpp
  navmap_mesh.cpp
)
target_compile_features(parser_navmap_core PUBLIC cxx_std_20)
target_compile_options(parser_navmap_core PRIVATE ${PJ_WARNING_FLAGS})
set_target_properties(parser_navmap_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
target_include_directories(parser_navmap_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# nanocdr is vendored verbatim under contrib/ (see contrib/nanocdr/README.md);
# SYSTEM keeps the strict warning set off it. Its constexpr endianness probe
# reads an inactive union member, which Clang rejects by default.
target_include_directories(parser_navmap_core SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/contrib)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(parser_navmap_core PUBLIC -Wno-invalid-constexpr)
endif()
target_link_libraries(parser_navmap_core PUBLIC plotjuggler_sdk::plugin_sdk)

# --- Plugin shared library ---
add_library(parser_navmap_plugin SHARED navmap_parser.cpp)
target_compile_features(parser_navmap_plugin PRIVATE cxx_std_20)
target_compile_options(parser_navmap_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(parser_navmap_plugin PRIVATE
  parser_navmap_core plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json)

pj_embed_manifest(parser_navmap_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/navmap_manifest.hpp
  VAR_NAME kNavMapManifest
)

pj_emit_plugin_manifest(parser_navmap_plugin
  FAMILY        message_parser
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# The object route hands sdk::BuiltinObject (std::any) to the host through the
# in-process C++ plugin pointer. On macOS, hidden RTTI would give the concrete
# builtin structs a distinct type identity in the dylib and std::any_cast in the
# host would fail; export RTTI there as parser_ros does.
if(APPLE)
  set_target_properties(parser_navmap_plugin parser_navmap_core PROPERTIES
    CXX_VISIBILITY_PRESET default
    C_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
  )
endif()

# --- Tests: the real .so through the host loader, plus the decoder directly ---
add_executable(navmap_parser_test tests/navmap_parser_test.cpp)
target_compile_features(navmap_parser_test PRIVATE cxx_std_20)
target_compile_options(navmap_parser_test PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(navmap_parser_test PRIVATE
  PJ_NAVMAP_PARSER_PLUGIN_PATH="$<TARGET_FILE:parser_navmap_plugin>"
  PJ_NAVMAP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
target_include_directories(navmap_parser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(navmap_parser_test PRIVATE
  parser_navmap_core
  plotjuggler_sdk::plugin_sdk
  plotjuggler_sdk::plugin_host
  GTest::gtest_main
)
add_test(NAME navmap_parser_test COMMAND navmap_parser_test)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(parser_navmap_plugin
  REQUIRED_EXPORTS PJ_get_message_parser_vtable)
