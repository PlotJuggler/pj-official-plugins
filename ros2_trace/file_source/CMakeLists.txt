find_package(nlohmann_json REQUIRED)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/EmbedManifest.cmake)

# Plugin shared library — thin adapter: runs FsTraceSource through the Pipeline
# and writes derived Samples to the host. No Qt; links the ros2_trace_model lib
# (which pulls libbabeltrace2 transitively for the CTF reader).
add_library(ros2_trace_file_source SHARED trace_file_source.cpp)
target_compile_features(ros2_trace_file_source PRIVATE cxx_std_20)
target_compile_options(ros2_trace_file_source PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(ros2_trace_file_source PRIVATE
  plotjuggler_core::plugin_sdk
  ros2_trace_model
  nlohmann_json::nlohmann_json
)

pj_embed_manifest(ros2_trace_file_source
  HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/trace_manifest.hpp
  VAR_NAME kTraceManifest
)
target_include_directories(ros2_trace_file_source PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# v4 plugin discovery: emit a sidecar .pjmanifest.json the host scans pre-dlopen.
pj_emit_plugin_manifest(ros2_trace_file_source
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
