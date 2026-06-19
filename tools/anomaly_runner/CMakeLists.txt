# anomaly_runner — headless Anomaly Detection CLI (Task F). Links the SDK plugin
# host (to load a DataSource plugin and capture its series in-memory) + the shared
# anomaly_core engine. SDK-only: no pj_datastore, no Qt.

# Pull in the shared engine, which lives under the sibling toolbox plugin. Guarded
# so the aggregate build (where the toolbox already added it) doesn't add it twice.
if(NOT TARGET anomaly_core)
  add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../toolbox_anomaly_detector/core
    ${CMAKE_CURRENT_BINARY_DIR}/anomaly_core)
endif()

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
)
