from conan import ConanFile


class ToolboxColormapConan(ConanFile):
    name = "toolbox_colormap"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "plotjuggler_core/[~0.3]",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "lua/5.4.6",
        "sol2/3.5.0",
    )
    default_options = {
        "*:shared": False,
        "lua/*:compile_as_cpp": True,
    }
