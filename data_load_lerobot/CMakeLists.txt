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
  dataset_shards_v3.cpp
  flatten_plan.cpp
  lerobot_video_window.cpp
)
target_compile_features(lerobot_source_plugin PRIVATE cxx_std_20)
target_compile_options(lerobot_source_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(lerobot_source_plugin PRIVATE
  plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json
  Arrow::arrow_static Parquet::parquet_static
  pj_arrow_helpers
  pj_video_demux  # lazy per-frame VideoFrame emission (pulls ffmpeg transitively)
)
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

add_executable(lerobot_dataset_model_test tests/dataset_model_test.cpp dataset_model.cpp dataset_shards_v3.cpp)
target_compile_features(lerobot_dataset_model_test PRIVATE cxx_std_20)
target_compile_options(lerobot_dataset_model_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(lerobot_dataset_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lerobot_dataset_model_test PRIVATE
  plotjuggler_sdk::plugin_sdk plotjuggler_sdk::plugin_host nlohmann_json::nlohmann_json
  Arrow::arrow_static Parquet::parquet_static GTest::gtest_main
)
add_test(NAME lerobot_dataset_model_test COMMAND lerobot_dataset_model_test)

# v3.0 fixture test — exercises the Arrow-backed meta/episodes reader against
# a synthetic, real-Parquet dataset built in-process.
add_executable(lerobot_dataset_model_v3_test tests/dataset_model_v3_test.cpp dataset_model.cpp dataset_shards_v3.cpp)
target_compile_features(lerobot_dataset_model_v3_test PRIVATE cxx_std_20)
target_compile_options(lerobot_dataset_model_v3_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(lerobot_dataset_model_v3_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lerobot_dataset_model_v3_test PRIVATE
  plotjuggler_sdk::plugin_sdk plotjuggler_sdk::plugin_host nlohmann_json::nlohmann_json
  Arrow::arrow_static Parquet::parquet_static GTest::gtest_main
)
add_test(NAME lerobot_dataset_model_v3_test COMMAND lerobot_dataset_model_v3_test)

add_executable(lerobot_flatten_plan_test tests/flatten_plan_test.cpp flatten_plan.cpp)
target_compile_features(lerobot_flatten_plan_test PRIVATE cxx_std_20)
target_compile_options(lerobot_flatten_plan_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(lerobot_flatten_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lerobot_flatten_plan_test PRIVATE
  plotjuggler_sdk::plugin_sdk plotjuggler_sdk::plugin_host nlohmann_json::nlohmann_json GTest::gtest_main
)
add_test(NAME lerobot_flatten_plan_test COMMAND lerobot_flatten_plan_test)

# Pure video-window resolution test: v2.x whole-file vs v3.0 keyframe-seek-back +
# presentation window + rebase. No host, no libav — pj_video_demux's header
# supplies the AccessUnit POD; plugin_sdk supplies its pj_base includes.
add_executable(lerobot_video_window_test tests/video_window_test.cpp lerobot_video_window.cpp)
target_compile_features(lerobot_video_window_test PRIVATE cxx_std_20)
target_compile_options(lerobot_video_window_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(lerobot_video_window_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../common/pj_video_demux/include
)
target_link_libraries(lerobot_video_window_test PRIVATE
  plotjuggler_sdk::plugin_sdk GTest::gtest_main
)
add_test(NAME lerobot_video_window_test COMMAND lerobot_video_window_test)

# Dialog/fanout serialization test: needs dataset_model.cpp because
# LeRobotDialog::loadModel() resolves to lerobot::loadDatasetModel; plugin_sdk
# supplies the DialogPluginTyped + WidgetData base classes.
add_executable(lerobot_dialog_fanout_test tests/dialog_fanout_test.cpp dataset_model.cpp dataset_shards_v3.cpp)
target_compile_features(lerobot_dialog_fanout_test PRIVATE cxx_std_20)
target_compile_options(lerobot_dialog_fanout_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(lerobot_dialog_fanout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lerobot_dialog_fanout_test PRIVATE
  plotjuggler_sdk::plugin_sdk plotjuggler_sdk::plugin_host nlohmann_json::nlohmann_json
  Arrow::arrow_static Parquet::parquet_static GTest::gtest_main
)
# Pull in the generated headers (dialog_lerobot_ui.hpp, lerobot_manifest.hpp)
# that lerobot_dialog.hpp transitively includes.
target_include_directories(lerobot_dialog_fanout_test PRIVATE
  $<TARGET_PROPERTY:lerobot_source_plugin,INCLUDE_DIRECTORIES>
)
add_test(NAME lerobot_dialog_fanout_test COMMAND lerobot_dialog_fanout_test)
