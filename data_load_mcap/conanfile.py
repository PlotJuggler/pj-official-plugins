from conan import ConanFile


class DataLoadMcapConan(ConanFile):
    name = "data_load_mcap"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "plotjuggler_core/[>=0.4.1 <0.5.0]",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "mcap/2.1.1",
    )
    default_options = {"*:shared": False}
