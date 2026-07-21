import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class DataStreamLslConan(ConanFile):
    name = "data_stream_lsl"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "liblsl/1.16.2",
    )
    # liblsl pulls in Boost. Disable Boost's test/cobalt modules (matching the
    # aggregate root recipe): otherwise Boost.Test's test_exec_monitor static lib
    # is dragged into every executable link and fails with an undefined
    # `test_main` reference.
    default_options = {
        "*:shared": False,
        "boost/*:without_test": True,
        "boost/*:without_cobalt": True,
    }
