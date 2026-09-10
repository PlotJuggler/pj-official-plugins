import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class ParserArrowConan(ConanFile):
    name = "parser_arrow"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "nanoarrow/0.7.0",
        "arrow/23.0.1",
    )
    default_options = {
        "*:shared": False,
        "arrow/*:parquet": True,
        "arrow/*:with_snappy": True,
        "arrow/*:with_mimalloc": False,
        "arrow/*:with_lz4": True,
        "arrow/*:with_zstd": True,
        "boost/*:without_test": True,
        "boost/*:without_cobalt": True,
    }
