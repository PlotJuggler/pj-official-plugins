from conan import ConanFile


class DataStreamRos2Conan(ConanFile):
    name = "data_stream_ros2"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # rclcpp / rosidl_typesupport_cpp / rosidl_typesupport_introspection_cpp
    # come from the ROS overlay (osrf/ros image), not Conan.
    requires = (
        "plotjuggler_core/[~0.3]",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
    )
    default_options = {"*:shared": False}
