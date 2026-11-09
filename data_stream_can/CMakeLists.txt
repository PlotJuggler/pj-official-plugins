# Live CAN streaming through QtSerialBus. The one non-widget plugin that links Qt (Core + SerialBus
# only), see cmake/PjHostQt.cmake. can_bus_reader.cpp is the only translation unit that includes a
# Qt header; everything else stays Qt-free.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/PjHostQt.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)

find_package(nlohmann_json REQUIRED)
pj_find_host_qt(COMPONENTS Core SerialBus SerialPort)

add_library(can_stream_core STATIC can_bus_reader.cpp)
set_target_properties(can_stream_core PROPERTIES
  AUTOMOC ON
  POSITION_INDEPENDENT_CODE ON
)
target_compile_features(can_stream_core PUBLIC cxx_std_20)
target_compile_options(can_stream_core PRIVATE ${PJ_WARNING_FLAGS})
target_compile_definitions(can_stream_core PRIVATE QT_NO_KEYWORDS)
target_include_directories(can_stream_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# plugin_sdk (INTERFACE: pj_base + nlohmann_json) for PJ::sdk::DrainQueue / LatestValueSlot.
target_link_libraries(can_stream_core
  PUBLIC plotjuggler_sdk::plugin_sdk
  PRIVATE Qt6::Core Qt6::SerialBus ${CMAKE_DL_LIBS}
)

# The plugin: CanSource (StreamSourceBase) + CanDialog, decoding through the shared pj_can_dbc.
add_library(can_stream_plugin SHARED can_source.cpp can_dialog.cpp)
target_compile_features(can_stream_plugin PRIVATE cxx_std_20)
target_compile_options(can_stream_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(can_stream_plugin PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(can_stream_plugin PRIVATE
  can_stream_core
  pj_can_dbc
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json)

pj_embed_manifest(can_stream_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/can_stream_manifest.hpp
  VAR_NAME kCanStreamManifest
)
pj_emit_plugin_manifest(can_stream_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
pj_embed_ui(can_stream_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/datastream_can.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/datastream_can_ui.hpp
  VAR_NAME kDataStreamCanUi
)
pj_harden_plugin_exports(can_stream_plugin
  REQUIRED_EXPORTS PJ_get_data_source_vtable PJ_get_dialog_vtable
  ALLOW_QT)

if(TARGET GTest::gtest AND BUILD_TESTING)
  # The Qt-free helpers.
  add_executable(can_stream_helpers_test tests/timestamp_anchor_test.cpp tests/read_error_streak_test.cpp)
  target_compile_features(can_stream_helpers_test PRIVATE cxx_std_20)
  target_compile_options(can_stream_helpers_test PRIVATE ${PJ_WARNING_FLAGS})
  target_include_directories(can_stream_helpers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(can_stream_helpers_test PRIVATE GTest::gtest_main)
  add_test(NAME can_stream_helpers_test COMMAND can_stream_helpers_test)

  # Own main(): the reader needs a QCoreApplication. One process per test: VirtualCAN's server is a
  # process-wide Qt global, and each test must meet it as a fresh process does. It listens on the
  # fixed TCP ports 35468 (and 35469 for the server-loss test), so the tests must not run in parallel.
  add_executable(can_bus_reader_test tests/can_bus_reader_test.cpp)
  target_compile_features(can_bus_reader_test PRIVATE cxx_std_20)
  target_compile_options(can_bus_reader_test PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(can_bus_reader_test PRIVATE can_stream_core Qt6::Core Qt6::SerialBus GTest::gtest)
  include(GoogleTest)
  gtest_discover_tests(can_bus_reader_test PROPERTIES RUN_SERIAL TRUE)

  # The dialog, and the built plugin driven end to end over VirtualCAN (own main(): a QCoreApplication,
  # as in PlotJuggler). Same one-process-per-test, run-serial rule as can_bus_reader_test.
  add_executable(can_stream_plugin_test tests/can_dialog_test.cpp tests/can_source_test.cpp can_dialog.cpp)
  target_compile_features(can_stream_plugin_test PRIVATE cxx_std_20)
  target_compile_options(can_stream_plugin_test PRIVATE ${PJ_WARNING_FLAGS})
  target_compile_definitions(can_stream_plugin_test PRIVATE
    CAN_STREAM_PLUGIN_PATH="$<TARGET_FILE:can_stream_plugin>")
  add_dependencies(can_stream_plugin_test can_stream_plugin)
  # can_dialog.cpp includes the headers generated for the plugin; common/can_dbc/tests has CaptureHost.
  target_include_directories(can_stream_plugin_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_LIST_DIR}/../common/can_dbc/tests)
  target_link_libraries(can_stream_plugin_test PRIVATE
    can_stream_core
    pj_can_dbc
    plotjuggler_sdk::plugin_host
    nlohmann_json::nlohmann_json
    Qt6::Core Qt6::SerialBus
    GTest::gtest)
  gtest_discover_tests(can_stream_plugin_test PROPERTIES RUN_SERIAL TRUE)
endif()
