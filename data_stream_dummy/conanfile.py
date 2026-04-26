from conan import ConanFile


class DataStreamDummyConan(ConanFile):
    name = "data_stream_dummy"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = ("gtest/1.17.0",)
    default_options = {"*:shared": False}
