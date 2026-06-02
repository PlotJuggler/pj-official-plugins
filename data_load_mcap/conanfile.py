import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class DataLoadMcapConan(ConanFile):
    name = "data_load_mcap"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # mcap headers are vendored in contrib/mcap/ (from the parallel-reader
    # fork). We still need lz4 and zstd for chunk decompression — previously
    # transitively from the mcap/ package, now declared directly here.
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "lz4/1.10.0",
        "zstd/1.5.7",
    )
    default_options = {"*:shared": False}
