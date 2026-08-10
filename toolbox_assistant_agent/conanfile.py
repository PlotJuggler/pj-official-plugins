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
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "fmt/12.1.0",
        # ixwebsocket provides both ix::HttpClient (Ollama /api/chat) and
        # ix::HttpServer (the localhost MCP bridge for the Claude Code backend).
        "ixwebsocket/11.4.6",
    )
    default_options = {"*:shared": False}
