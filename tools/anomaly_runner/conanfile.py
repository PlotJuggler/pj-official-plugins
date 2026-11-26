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
    # anomaly_core delegates detection to the shared Luau engine pj_scripting_core (it carries Luau and
    # kissfft). libcurl powers the opt-in notification sinks (webhook POST + email SMTP); gtest builds the
    # notify tests.
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "pj_scripting_core/0.2.0",
        "nlohmann_json/3.12.0",
        "libcurl/8.23.0",
        "gtest/1.17.0",
    )
    default_options = {
        "*:shared": False,
        # Keep the Luau binary id consistent with the (PIC) plugin build.
        "luau/*:fPIC": True,
    }
