import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class DataLoadCandumpConan(ConanFile):
    name = "data_load_candump"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # No vendored/CPM dependency of its own: candump text is parsed by hand
    # (no zlib/lblf-style binary container), and CAN decoding (dbc_parser_cpp
    # + fast_float via CPM) comes through the shared common/can_dbc lib, so
    # it is not listed here (same pattern as data_load_blf/data_load_mf4).
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
    )
    default_options = {"*:shared": False}
