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
    # The runner compiles anomaly_core, which delegates detection to the shared Luau
    # engine pj_scripting_core (Luau/kissfft are absorbed into it, not direct deps).
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "pj_scripting_core/0.1.0",
        "nlohmann_json/3.12.0",
    )
    default_options = {
        "*:shared": False,
        # Keep the Luau binary id consistent with the (PIC) plugin build.
        "luau/*:fPIC": True,
    }
