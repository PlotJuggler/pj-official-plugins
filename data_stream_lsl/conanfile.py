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

    # liblsl pulls in Boost. Disable Boost's test/cobalt modules (matching the
    # aggregate root recipe): otherwise Boost.Test's test_exec_monitor static lib
    # is dragged into every executable link and fails with an undefined
    # `test_main` reference.
    default_options = {
        "*:shared": False,
        "boost/*:without_test": True,
        "boost/*:without_cobalt": True,
    }

    def requirements(self):
        self.requires(f"plotjuggler_sdk/{_SDK_VERSION}")
        self.requires("gtest/1.17.0")
        self.requires("nlohmann_json/3.12.0")
        self.requires("liblsl/1.16.2")
        # liblsl depends on pugixml/1.13, whose upstream CMakeLists declares a
        # pre-3.5 cmake_minimum_required. CMake 4.x (which Conan uses to build
        # the package from source in CI, where no prebuilt pugixml binary
        # matches) removed compatibility with < 3.5 and fails to configure it.
        # Override to 1.15, whose CMakeLists uses a modern minimum. pugixml's API
        # is stable across these, so liblsl is unaffected.
        self.requires("pugixml/1.15", override=True)
