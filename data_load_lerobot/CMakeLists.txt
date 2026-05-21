find_package(nlohmann_json REQUIRED)
if(NOT TARGET Arrow::arrow_static)
  find_package(Arrow REQUIRED)
endif()
if(NOT TARGET Parquet::parquet_static)
  find_package(Parquet REQUIRED)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

# --- Plugin shared library ---
add_library(lerobot_source_plugin SHARED
  lerobot_plugin.cpp
  dataset_model.cpp
  flatten_plan.cpp
)
target_compile_features(lerobot_source_plugin PRIVATE cxx_std_20)
target_compile_options(lerobot_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(lerobot_source_plugin PRIVATE
  pj_base pj_dialog_sdk nlohmann_json::nlohmann_json
  Arrow::arrow_static Parquet::parquet_static
)
# This plugin statically embeds Arrow. Without this, its symbols (arrow::*)
# are exported from the .so and interpose on the host's own copies once the
# extension manager dlopen()s us — a buffer allocated by one copy then freed
# by another aborts with "munmap_chunk(): invalid pointer" during plugin
# scanning. Give every static-archive symbol local binding so only our
# PJ_get_*_vtable entry points (from our own objects) stay exported.
#
# --exclude-libs is GNU ld / lld / gold only. macOS ld64 and MSVC's link.exe
# don't understand it (and don't need it: default static-archive visibility
# differs on those platforms, so the interposition problem doesn't arise).
if(NOT APPLE AND NOT WIN32)
  target_link_options(lerobot_source_plugin PRIVATE LINKER:--exclude-libs,ALL)
endif()

pj_embed_ui(lerobot_source_plugin
  UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/dialog_lerobot.ui
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/dialog_lerobot_ui.hpp
  VAR_NAME kDialogLerobotUi
)

pj_embed_manifest(lerobot_source_plugin
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/lerobot_manifest.hpp
  VAR_NAME kLerobotManifest
)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host can scan
# pre-dlopen. Content derived from manifest.json + {abi_major, family}.
pj_emit_plugin_manifest(lerobot_source_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# --- Unit tests ---
# Tests live under tests/ but #include headers from the plugin root, so each
# target gets ${CMAKE_CURRENT_SOURCE_DIR} on its include path.
function(lerobot_add_test name)
  add_executable(${name} ${ARGN})
  target_compile_features(${name} PRIVATE cxx_std_20)
  target_compile_options(${name} PRIVATE ${PJ_WARNING_FLAGS})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE pj_base nlohmann_json::nlohmann_json GTest::gtest_main)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

lerobot_add_test(lerobot_dataset_model_test tests/dataset_model_test.cpp dataset_model.cpp)
lerobot_add_test(lerobot_flatten_plan_test  tests/flatten_plan_test.cpp  flatten_plan.cpp)

# Dialog/fanout serialization test: needs dataset_model.cpp because
# LeRobotDialog::loadModel() resolves to lerobot::loadDatasetModel; pj_dialog_sdk
# supplies the DialogPluginTyped + WidgetData base classes.
add_executable(lerobot_dialog_fanout_test tests/dialog_fanout_test.cpp dataset_model.cpp)
target_compile_features(lerobot_dialog_fanout_test PRIVATE cxx_std_20)
target_compile_options(lerobot_dialog_fanout_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(lerobot_dialog_fanout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lerobot_dialog_fanout_test PRIVATE
  pj_base pj_dialog_sdk nlohmann_json::nlohmann_json GTest::gtest_main
)
# Pull in the generated headers (dialog_lerobot_ui.hpp, lerobot_manifest.hpp)
# that lerobot_dialog.hpp transitively includes.
target_include_directories(lerobot_dialog_fanout_test PRIVATE
  $<TARGET_PROPERTY:lerobot_source_plugin,INCLUDE_DIRECTORIES>
)
add_test(NAME lerobot_dialog_fanout_test COMMAND lerobot_dialog_fanout_test)
