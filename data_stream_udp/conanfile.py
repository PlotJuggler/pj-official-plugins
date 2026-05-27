from conan import ConanFile


class DataStreamUdpConan(ConanFile):
    name = "data_stream_udp"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "plotjuggler_core/[>=0.4.1 <0.5.0]",
        "gtest/1.17.0",
        "asio/1.28.2",
        "nlohmann_json/3.12.0",
    )
    default_options = {"*:shared": False}
