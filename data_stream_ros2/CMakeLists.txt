# ---------------------------------------------------------------------------
# data_stream_ros2 — ROS 2 topic subscriber marketplace extension
#
# Two build targets live in this directory:
#
#   ros2_stream_plugin  (proxy/src/)
#     Distro dispatch shim with no rclcpp dependency. Always built.
#     Entry point advertised by the marketplace manifest. Detects the
#     installed ROS 2 distribution at load time and dlopen-s the
#     matching per-distro binary.
#     Output: libros2_stream_plugin.so
#
#   ros2_stream_plugin_distro  (distro/src/)
#     Actual subscriber compiled against a specific ROS 2 distribution.
#     Opt-in: only built when -DPJ_BUILD_ROS2_DISTRO=ON, inside a
#     ROS-aware environment (osrf/ros Docker container).
#     Output: libros2_stream_plugin-<distro>.so  (e.g. -humble, -jazzy)
#
# Typical CI invocations:
#   proxy : cmake ... -DPJ_BUILD_PLUGIN=data_stream_ros2
#   distro : cmake ... -DPJ_BUILD_PLUGIN=data_stream_ros2 -DPJ_BUILD_ROS2_DISTRO=ON
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
    plotjuggler_sdk::plugin_sdk
    ${CMAKE_DL_LIBS}
)

set_target_properties(ros2_stream_plugin PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

pj_emit_plugin_manifest(ros2_stream_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# ---------------------------------------------------------------------------
# Per-distro target — per-distro ROS 2 subscriber, opt-in
# ---------------------------------------------------------------------------

if(PJ_BUILD_ROS2_DISTRO)
  # Read distro from environment (set by source /opt/ros/<distro>/setup.bash).
  # Can also be overridden explicitly with -DROS_DISTRO=<distro>.
  if(NOT DEFINED ROS_DISTRO)
    set(ROS_DISTRO "$ENV{ROS_DISTRO}")
  endif()
  if(NOT ROS_DISTRO)
    message(FATAL_ERROR
      "PJ_BUILD_ROS2_DISTRO=ON but ROS_DISTRO is not set. "
      "Source /opt/ros/<distro>/setup.bash first, or pass -DROS_DISTRO=<distro>.")
  endif()

  find_package(rclcpp REQUIRED)
  find_package(rosidl_typesupport_cpp REQUIRED)
  find_package(rosidl_typesupport_introspection_cpp REQUIRED)
  find_package(nlohmann_json REQUIRED)

  include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
  include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedUi.cmake)

  add_library(ros2_stream_plugin_distro SHARED distro/src/data_stream_ros2.cpp)
  target_compile_features(ros2_stream_plugin_distro PRIVATE cxx_std_20)
  target_compile_options(ros2_stream_plugin_distro PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(ros2_stream_plugin_distro
    PRIVATE
      plotjuggler_sdk::plugin_sdk
      nlohmann_json::nlohmann_json
      rclcpp::rclcpp
      rosidl_typesupport_cpp::rosidl_typesupport_cpp
      rosidl_typesupport_introspection_cpp::rosidl_typesupport_introspection_cpp
      pj_array_policy
  )

  pj_embed_manifest(ros2_stream_plugin_distro
    MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
    HEADER        ${CMAKE_CURRENT_BINARY_DIR}/generated/ros2_manifest.hpp
    VAR_NAME      kRos2Manifest
  )

  pj_embed_ui(ros2_stream_plugin_distro
    UI_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/distro/datastream_ros2.ui
    HEADER   ${CMAKE_CURRENT_BINARY_DIR}/generated/datastream_ros2_ui.hpp
    VAR_NAME kDataStreamRos2Ui
  )

  set_target_properties(ros2_stream_plugin_distro PROPERTIES
    OUTPUT_NAME "ros2_stream_plugin-${ROS_DISTRO}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  )

  # Expose the active distro as a compile macro (e.g. ROS_DISTRO_HUMBLE,
  # ROS_DISTRO_JAZZY) so source code can branch on the version when an
  # API differs across distros (see distro/src/ros2_schema_builder.hpp
  # for the rosbag2_cpp -> rclcpp typesupport migration).
  string(TOUPPER "${ROS_DISTRO}" ROS_DISTRO_UPPER)
  target_compile_definitions(ros2_stream_plugin_distro PRIVATE
    "ROS_DISTRO_${ROS_DISTRO_UPPER}"
  )

  pj_emit_plugin_manifest(ros2_stream_plugin_distro
    FAMILY        data_source
    MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
  )
endif()
