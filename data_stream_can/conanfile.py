import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class DataStreamCanConan(ConanFile):
    name = "data_stream_can"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # Qt (Core + SerialBus) is deliberately NOT a Conan requirement: the plugin must be built
    # against the official Qt matching the host's major.minor (QT_VERSION, cmake/PjHostQt.cmake).
    # CAN decoding (dbc_parser_cpp + fast_float via CPM) comes through common/can_dbc.
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
    )
    default_options = {"*:shared": False}
