from conan import ConanFile


class DataStreamMqttConan(ConanFile):
    name = "data_stream_mqtt"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "plotjuggler_core/0.2.1",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "paho-mqtt-cpp/1.5.3",
    )
    default_options = {"*:shared": False}
