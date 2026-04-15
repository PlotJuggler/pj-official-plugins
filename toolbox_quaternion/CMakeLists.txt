find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

add_library(toolbox_quaternion_plugin SHARED quaternion_plugin.cpp)
target_compile_features(toolbox_quaternion_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_quaternion_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_quaternion_plugin PRIVATE
  pj_base
  pj_dialog_sdk
  nlohmann_json::nlohmann_json
)

pj_embed_manifest(toolbox_quaternion_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/quaternion_manifest.hpp
  VAR_NAME kQuaternionManifest
)

pj_embed_ui(toolbox_quaternion_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/quaternion_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/quaternion_dialog_ui.hpp
  VAR_NAME kQuaternionDialogUi
)

if(TARGET GTest::gtest_main)
  add_executable(quaternion_plugin_test quaternion_plugin_test.cpp)
  target_compile_features(quaternion_plugin_test PRIVATE cxx_std_20)
  target_compile_options(quaternion_plugin_test PRIVATE ${PJ_WARNING_FLAGS})
  target_compile_definitions(quaternion_plugin_test PRIVATE
    PJ_QUATERNION_PLUGIN_PATH="$<TARGET_FILE:toolbox_quaternion_plugin>"
  )
  target_link_libraries(quaternion_plugin_test PRIVATE
    pj_toolbox_host
    pj_base
    GTest::gtest_main
  )
  add_test(NAME quaternion_plugin_test COMMAND quaternion_plugin_test)
endif()
