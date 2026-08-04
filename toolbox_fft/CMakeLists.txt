find_package(nlohmann_json REQUIRED)
find_package(kissfft REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

add_library(toolbox_fft_algorithm STATIC fft_algorithm.cpp)
set_target_properties(toolbox_fft_algorithm PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(toolbox_fft_algorithm PUBLIC cxx_std_20)
target_compile_options(toolbox_fft_algorithm PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(toolbox_fft_algorithm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(toolbox_fft_algorithm PUBLIC kissfft::kissfft)

add_library(toolbox_fft_plugin SHARED fft_plugin.cpp)

target_compile_features(toolbox_fft_plugin PRIVATE cxx_std_20)
target_compile_options(toolbox_fft_plugin PRIVATE ${PJ_WARNING_FLAGS})

target_link_libraries(toolbox_fft_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk
  nlohmann_json::nlohmann_json
  toolbox_fft_algorithm
)

target_include_directories(toolbox_fft_plugin PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

pj_embed_ui(toolbox_fft_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/fft_dialog.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/fft_dialog_ui.hpp
  VAR_NAME kFftDialogUi
)

pj_embed_manifest(toolbox_fft_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/fft_manifest.hpp
  VAR_NAME kFftManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(toolbox_fft_plugin
  FAMILY        toolbox
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

if(TARGET GTest::gtest_main)
  add_executable(toolbox_fft_plugin_test fft_plugin_test.cpp)
  target_compile_features(toolbox_fft_plugin_test PRIVATE cxx_std_20)
  target_compile_options(toolbox_fft_plugin_test PRIVATE ${PJ_WARNING_FLAGS})
  target_compile_definitions(toolbox_fft_plugin_test PRIVATE
    PJ_FFT_PLUGIN_PATH="$<TARGET_FILE:toolbox_fft_plugin>"
  )
  add_dependencies(toolbox_fft_plugin_test toolbox_fft_plugin)
  target_link_libraries(toolbox_fft_plugin_test PRIVATE
    toolbox_fft_algorithm
    plotjuggler_sdk::plugin_host
    plotjuggler_sdk::plugin_sdk
    GTest::gtest_main
  )
  add_test(NAME toolbox_fft_plugin_test COMMAND toolbox_fft_plugin_test)
endif()
