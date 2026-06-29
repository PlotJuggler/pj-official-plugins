import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class ToolboxAnomalyDetectorConan(ConanFile):
    name = "toolbox_anomaly_detector"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # Host-driven, Luau-FREE plugin: detection runs in the HOST (the plugin submits the
    # rule via the SDK data-processors service). The .so links only the Luau-free
    # anomaly_helpers, so it needs neither pj_scripting_core nor a Lua VM — the engine
    # layer (anomaly_core + pj_scripting_core) is built only for the headless runner.
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "nlohmann_json/3.12.0",
    )
    default_options = {
        "*:shared": False,
    }
