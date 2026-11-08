import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class ToolboxAssistantAgentConan(ConanFile):
    name = "toolbox_assistant_agent"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        # GTest backs the plugin's unit tests; the root CMakeLists' find_package(GTest)
        # runs for every per-plugin build, so each plugin's recipe must provide it.
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
    )
    default_options = {
        "*:shared": False,
    }
