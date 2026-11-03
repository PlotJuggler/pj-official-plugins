import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class BasicWidgetsConan(ConanFile):
    name = "widget_basic"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # Qt is deliberately NOT a Conan requirement: a Widget plugin must be built against the
    # official Qt that matches the host's major.minor (see CMakeLists.txt), which CI installs
    # from the official installer, not from ConanCenter.
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "nlohmann_json/3.12.0",
        # The root CMakeLists' find_package(GTest) runs for every per-plugin build.
        "gtest/1.17.0",
    )
    default_options = {
        "*:shared": False,
    }
