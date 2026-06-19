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
    # Detection runs through the shared Luau engine pj_scripting_core (Luau + kissfft
    # are absorbed into it); the plugin no longer bundles its own sol2/Lua VM.
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "pj_scripting_core/0.1.0",
        "nlohmann_json/3.12.0",
    )
    default_options = {
        "*:shared": False,
        # The plugin is a shared lib (.so); Luau's static libs must be PIC to link in.
        "luau/*:fPIC": True,
    }
