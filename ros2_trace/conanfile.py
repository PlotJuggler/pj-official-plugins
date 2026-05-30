from conan import ConanFile


class Ros2TraceConan(ConanFile):
    name = "ros2_trace"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "plotjuggler_core/[~0.5]",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
    )
    default_options = {"*:shared": False}
    # NOTE: libbabeltrace2 is a system dependency (found via pkg-config), not a
    # Conan package; install it with the system package manager
    # (e.g. apt install libbabeltrace2-dev).
