import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class DataLoadMf4Conan(ConanFile):
    name = "data_load_mf4"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # mdflib itself is vendored via CPM (no Conan recipe); it needs zlib + expat,
    # which we provide from Conan so mdflib reuses them via find_package.
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "zlib/1.3.1",
        "expat/2.6.4",
    )
    default_options = {"*:shared": False}
