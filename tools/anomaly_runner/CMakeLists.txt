# anomaly_runner — headless Anomaly Detection CLI (Task F). Links the SDK plugin
# host (to load a DataSource plugin and capture its series in-memory) + the shared
# anomaly_core engine. SDK-only: no pj_datastore, no Qt.

# Pull in the shared engine, which lives under the sibling toolbox plugin. Guarded
# so the aggregate build (where the toolbox already added it) doesn't add it twice.
# Building the runner implies the engine layer, so enable it for the core subdir below
# (the GUI-only / aggregate build leaves PJ_BUILD_ANOMALY_RUNNER OFF and stays Luau-free).
set(PJ_BUILD_ANOMALY_RUNNER ON)
if(NOT TARGET anomaly_core)
  add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../toolbox_anomaly_detector/core
    ${CMAKE_CURRENT_BINARY_DIR}/anomaly_core)
endif()

# libcurl: webhook (HTTP POST) + email (SMTP) transports for the notification sinks.
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

# The notification layer (webhook/email/command sinks). Decoupled from anomaly_core:
# it operates on the report JSON text, so its unit test links only this + curl + json.
add_library(anomaly_notify STATIC notify.cpp)
target_compile_features(anomaly_notify PUBLIC cxx_std_20)
target_compile_options(anomaly_notify PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(anomaly_notify PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(anomaly_notify PUBLIC nlohmann_json::nlohmann_json PRIVATE CURL::libcurl)

add_executable(anomaly_runner main.cpp)
target_compile_features(anomaly_runner PRIVATE cxx_std_20)
target_compile_options(anomaly_runner PRIVATE ${PJ_WARNING_FLAGS})

# Co-locate the binary with the plugin .so files so the default datasource-plugin
# lookup (libcsv_source_plugin.so next to the executable) works out of the box.
set_target_properties(anomaly_runner PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})

target_link_libraries(anomaly_runner PRIVATE
  plotjuggler_sdk::plugin_host
  anomaly_core
  anomaly_notify
)

# Unit tests for the pure notification logic (config parse, policy, ${ENV}, command sink).
if(TARGET GTest::gtest_main)
  add_executable(notify_test tests/notify_test.cpp)
  target_compile_features(notify_test PRIVATE cxx_std_20)
  target_link_libraries(notify_test PRIVATE anomaly_notify GTest::gtest_main)
  add_test(NAME notify_test COMMAND notify_test)
endif()
