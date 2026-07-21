import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class DataLoadBlfConan(ConanFile):
    name = "data_load_blf"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # lblf (BLF reader) is vendored via CPM and needs zlib. The DBC decoder
    # (dbc_parser_cpp + fast_float) comes via CPM through the shared pj_can_dbc
    # common lib, so it is not listed here.
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "zlib/1.3.1",
    )
    default_options = {"*:shared": False}
