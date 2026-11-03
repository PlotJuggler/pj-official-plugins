find_package(nlohmann_json REQUIRED)
find_package(PahoMqttCpp REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(mqtt_source_plugin SHARED mqtt_source.cpp)
target_compile_features(mqtt_source_plugin PRIVATE cxx_std_20)
target_compile_options(mqtt_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(mqtt_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json PahoMqttCpp::paho-mqttpp3-static
)

pj_embed_ui(mqtt_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/datastream_mqtt.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/datastream_mqtt_ui.hpp
  VAR_NAME kDataStreamMqttUi
)

pj_embed_manifest(mqtt_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/mqtt_manifest.hpp
  VAR_NAME kMqttManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(mqtt_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Lifetime tests: the dialog against an in-process fake broker (POSIX only) ---
add_executable(mqtt_dialog_lifetime_test tests/mqtt_dialog_lifetime_test.cpp)
target_compile_features(mqtt_dialog_lifetime_test PRIVATE cxx_std_20)
target_compile_options(mqtt_dialog_lifetime_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(mqtt_dialog_lifetime_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(mqtt_dialog_lifetime_test PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json PahoMqttCpp::paho-mqttpp3-static GTest::gtest_main
)
add_test(NAME mqtt_dialog_lifetime_test COMMAND mqtt_dialog_lifetime_test)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
pj_harden_plugin_exports(mqtt_source_plugin
  REQUIRED_EXPORTS PJ_get_data_source_vtable PJ_get_dialog_vtable)
