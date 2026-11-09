# Live CAN streaming through QtSerialBus. The one non-widget plugin that links Qt (Core + SerialBus
# only), see cmake/PjHostQt.cmake. can_bus_reader.cpp is the only translation unit that includes a
# Qt header; everything else stays Qt-free.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/PjHostQt.cmake)

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

if(TARGET GTest::gtest AND BUILD_TESTING)
  add_executable(timestamp_anchor_test tests/timestamp_anchor_test.cpp)
  target_compile_features(timestamp_anchor_test PRIVATE cxx_std_20)
  target_compile_options(timestamp_anchor_test PRIVATE ${PJ_WARNING_FLAGS})
  target_include_directories(timestamp_anchor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(timestamp_anchor_test PRIVATE GTest::gtest_main)
  add_test(NAME timestamp_anchor_test COMMAND timestamp_anchor_test)

  # Own main(): the reader needs a QCoreApplication. One process per test: VirtualCAN's server is a
  # process-wide Qt global, and each test must meet it as a fresh process does. It listens on the
  # fixed TCP ports 35468 (and 35469 for the server-loss test), so the tests must not run in parallel.
  add_executable(can_bus_reader_test tests/can_bus_reader_test.cpp)
  target_compile_features(can_bus_reader_test PRIVATE cxx_std_20)
  target_compile_options(can_bus_reader_test PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(can_bus_reader_test PRIVATE can_stream_core Qt6::Core Qt6::SerialBus GTest::gtest)
  include(GoogleTest)
  gtest_discover_tests(can_bus_reader_test PROPERTIES RUN_SERIAL TRUE)
endif()
