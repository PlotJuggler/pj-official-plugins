from conan import ConanFile


class DataStreamFoxgloveBridgeConan(ConanFile):
    name = "data_stream_foxglove_bridge"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "plotjuggler_core/[>=0.4.1 <0.5.0]",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "ixwebsocket/11.4.6",
    )
    default_options = {"*:shared": False}
