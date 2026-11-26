# Two targets:
#
#   ros2_stream_plugin  (proxy/src/)
#     Distro dispatch shim with no rclcpp dependency; always built. It is the marketplace manifest's entry point:
#     it detects the installed ROS 2 distribution at load time and dlopen-s the matching per-distro binary.
#     Output: libros2_stream_plugin.so
#
#   ros2_stream_plugin_distro  (distro/src/)
#     The subscriber, compiled against one ROS 2 distribution. Built only with -DPJ_BUILD_ROS2_DISTRO=ON inside a
#     ROS environment (ros:<distro>-ros-base container, ROS_DISTRO set by sourcing /opt/ros/<distro>/setup.bash).
#     Output: libros2_stream_plugin-<distro>.so  (e.g. -humble, -jazzy)
#
#   proxy : cmake ... -DPJ_BUILD_PLUGIN=data_stream_ros2
#   distro: cmake ... -DPJ_BUILD_PLUGIN=data_stream_ros2 -DPJ_BUILD_ROS2_DISTRO=ON

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

# Embedded in the proxy so the host scanner can describe this extension without ROS installed; the inner per-distro
# binary is dlopen-ed only when the source is instantiated.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/EmbedManifest.cmake)
pj_embed_manifest(ros2_stream_plugin
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
  HEADER        ${CMAKE_CURRENT_BINARY_DIR}/generated/proxy/ros2_manifest.hpp
  VAR_NAME      kRos2ProxyManifest
)

pj_emit_plugin_manifest(ros2_stream_plugin
  FAMILY        data_source
  MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)

# ros2_topic_subscription.hpp has no rclcpp dependency, so this builds without PJ_BUILD_ROS2_DISTRO or a ROS 2
# environment.
add_executable(ros2_topic_subscription_test tests/ros2_topic_subscription_test.cpp)
target_compile_features(ros2_topic_subscription_test PRIVATE cxx_std_20)
target_compile_options(ros2_topic_subscription_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(ros2_topic_subscription_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ros2_topic_subscription_test PRIVATE
  plotjuggler_sdk::plugin_sdk
  pj_always_included
  GTest::gtest_main
)
add_test(NAME ros2_topic_subscription_test COMMAND ros2_topic_subscription_test)

# Optional-runtime admission without ROS, including a real dlopen failure whose missing dependency exists outside the
# loader search path.
add_library(ros2_proxy_test_dependency SHARED tests/proxy_dependency.cpp)
add_library(ros2_proxy_test_inner SHARED tests/proxy_inner.cpp)
target_compile_definitions(ros2_proxy_test_inner PRIVATE PJ_TEST_ROS2_DEPENDENCY)
target_link_libraries(ros2_proxy_test_inner PRIVATE ros2_proxy_test_dependency)
set_target_properties(ros2_proxy_test_inner PROPERTIES SKIP_BUILD_RPATH ON)
add_library(ros2_proxy_test_missing_exports SHARED tests/proxy_inner.cpp)
set_target_properties(ros2_proxy_test_dependency ros2_proxy_test_inner ros2_proxy_test_missing_exports PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures")

add_executable(ros2_proxy_test tests/ros2_proxy_test.cpp)
target_compile_features(ros2_proxy_test PRIVATE cxx_std_20)
target_compile_options(ros2_proxy_test PRIVATE ${PJ_WARNING_FLAGS})
target_include_directories(ros2_proxy_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/proxy")
target_link_libraries(ros2_proxy_test PRIVATE plotjuggler_sdk::plugin_host GTest::gtest_main ${CMAKE_DL_LIBS})
target_compile_definitions(ros2_proxy_test PRIVATE
  ROS2_TEST_INNER="$<TARGET_FILE:ros2_proxy_test_inner>"
  ROS2_TEST_DEPENDENCY="$<TARGET_FILE:ros2_proxy_test_dependency>"
  ROS2_TEST_MISSING_EXPORTS="$<TARGET_FILE:ros2_proxy_test_missing_exports>")
add_dependencies(ros2_proxy_test ros2_proxy_test_inner ros2_proxy_test_missing_exports)
add_test(NAME ros2_proxy_test COMMAND ros2_proxy_test)

if(PJ_BUILD_ROS2_DISTRO)
  # From the environment (source /opt/ros/<distro>/setup.bash), or -DROS_DISTRO=<distro>.
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
      pj_always_included
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

  # ROS_DISTRO_<UPPER> (e.g. ROS_DISTRO_HUMBLE) lets source branch where an API differs across distros (see
  # distro/src/ros2_schema_builder.hpp).
  string(TOUPPER "${ROS_DISTRO}" ROS_DISTRO_UPPER)
  target_compile_definitions(ros2_stream_plugin_distro PRIVATE
    "ROS_DISTRO_${ROS_DISTRO_UPPER}"
  )

  pj_emit_plugin_manifest(ros2_stream_plugin_distro
    FAMILY        data_source
    MANIFEST_FILE ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
  )
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/HardenPluginExports.cmake)
# The proxy also exports the PJ_get_proxy_last_error diagnostic hook (see proxy/src/data_stream_ros2.cpp); keep it in
# the allowlist and enforced by the gate alongside both family getters.
pj_harden_plugin_exports(ros2_stream_plugin
  REQUIRED_EXPORTS PJ_get_data_source_vtable PJ_get_dialog_vtable PJ_get_proxy_last_error
  EXTRA_EXPORTS PJ_get_proxy_last_error)

# The proxy dlopen-s the inner payload and resolves its PJ_ros2_inner_* getters (renamed so the host scanner never
# mistakes the payload for a top-level plugin). The shared allowlist would localize them, so they ride in as
# EXTRA_EXPORTS. ROS core libraries (rclcpp/rcl/rmw) are linked shared, so localizing the payload's own symbols does
# not affect RMW/typesupport symbol resolution.
if(PJ_BUILD_ROS2_DISTRO)
  pj_harden_plugin_exports(ros2_stream_plugin_distro
    REQUIRED_EXPORTS PJ_ros2_inner_get_data_source_vtable PJ_ros2_inner_get_dialog_vtable
    EXTRA_EXPORTS PJ_ros2_inner_get_data_source_vtable PJ_ros2_inner_get_dialog_vtable)
endif()
