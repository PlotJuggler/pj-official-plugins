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
        # ixwebsocket provides ix::HttpServer (the loopback MCP bridge the
        # Claude Code / Codex CLIs call back into) plus the ix::HttpClient the
        # MCP tests drive it with. Static, with its mbedtls/zlib closure, so
        # the released zip is self-contained (same as data_stream_foxglove_bridge).
        "ixwebsocket/11.4.6",
    )
    default_options = {
        "*:shared": False,
    }
