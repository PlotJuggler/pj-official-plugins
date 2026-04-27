# ---------------------------------------------------------------------------
# data_stream_ros2 — ROS 2 topic subscriber marketplace extension
#
# Two build targets live in this directory:
#
#   ros2_stream_plugin  (proxy/src/)
#     Distro dispatch shim with no rclcpp dependency. Always built.
#     Entry point advertised by the marketplace manifest. Detects the
#     installed ROS 2 distribution at load time and dlopen-s the
#     matching per-distro inner binary.
#     Output: libros2_stream_plugin.so
#
#   ros2_stream_plugin_inner  (inner/src/)
#     Actual subscriber compiled against a specific ROS 2 distribution.
#     Opt-in: only built when -DPJ_BUILD_ROS2_INNER=ON, inside a
#     ROS-aware environment (osrf/ros Docker container).
#     Output: libros2_stream_plugin-<distro>.so  (e.g. -humble, -jazzy)
#
# Typical CI invocations:
#   proxy : cmake ... -DPJ_BUILD_PLUGIN=data_stream_ros2
#   inner : cmake ... -DPJ_BUILD_PLUGIN=data_stream_ros2 -DPJ_BUILD_ROS2_INNER=ON
#           (requires ROS_DISTRO to be set via source /opt/ros/<distro>/setup.bash)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Proxy target — entry point, no ROS dependency, always built
# ---------------------------------------------------------------------------

add_library(ros2_stream_plugin SHARED proxy/src/data_stream_ros2.cpp)
target_compile_features(ros2_stream_plugin PRIVATE cxx_std_20)
target_compile_options(ros2_stream_plugin PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(ros2_stream_plugin
  PRIVATE
    pj_base
    ${CMAKE_DL_LIBS}
)

set_target_properties(ros2_stream_plugin PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# Inner target — per-distro ROS 2 subscriber, opt-in
# ---------------------------------------------------------------------------

if(PJ_BUILD_ROS2_INNER)
  # Read distro from environment (set by source /opt/ros/<distro>/setup.bash).
  # Can also be overridden explicitly with -DROS_DISTRO=<distro>.
  if(NOT DEFINED ROS_DISTRO)
    set(ROS_DISTRO "$ENV{ROS_DISTRO}")
  endif()
  if(NOT ROS_DISTRO)
    message(FATAL_ERROR
      "PJ_BUILD_ROS2_INNER=ON but ROS_DISTRO is not set. "
      "Source /opt/ros/<distro>/setup.bash first, or pass -DROS_DISTRO=<distro>.")
  endif()

  find_package(rclcpp REQUIRED)
  find_package(rosidl_typesupport_cpp REQUIRED)
  find_package(nlohmann_json REQUIRED)

  include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)

  add_library(ros2_stream_plugin_inner SHARED inner/src/data_stream_ros2.cpp)
  target_compile_features(ros2_stream_plugin_inner PRIVATE cxx_std_20)
  target_compile_options(ros2_stream_plugin_inner PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(ros2_stream_plugin_inner
    PRIVATE
      pj_base
      pj_dialog_sdk
      nlohmann_json::nlohmann_json
      rclcpp::rclcpp
      rosidl_typesupport_cpp::rosidl_typesupport_cpp
  )

  pj_embed_manifest(ros2_stream_plugin_inner
    MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
    HEADER        ${CMAKE_CURRENT_BINARY_DIR}/generated/ros2_manifest.hpp
    VAR_NAME      kRos2Manifest
  )

  set_target_properties(ros2_stream_plugin_inner PROPERTIES
    OUTPUT_NAME "ros2_stream_plugin-${ROS_DISTRO}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  )
endif()
