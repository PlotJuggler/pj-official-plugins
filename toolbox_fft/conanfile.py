from conan import ConanFile


class ToolboxFftConan(ConanFile):
    name = "toolbox_fft"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "plotjuggler_core/[>=0.4.1 <0.5.0]",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "kissfft/131.1.0",
    )
    default_options = {"*:shared": False}
