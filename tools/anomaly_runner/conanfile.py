import os
from conan import ConanFile


_SDK_VERSION = (
    open(
        os.path.join(
            os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir, "SDK_VERSION"
        )
    )
    .read()
    .strip()
)


class AnomalyRunnerConan(ConanFile):
    name = "anomaly_runner"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # The runner compiles anomaly_core, so it needs the engine's deps too.
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "lua/5.4.6",
        "sol2/3.5.0",
        "kissfft/131.1.0",
        "nlohmann_json/3.12.0",
    )
    default_options = {
        "*:shared": False,
        # sol2 needs Lua built with C++ linkage.
        "lua/*:compile_as_cpp": True,
    }
