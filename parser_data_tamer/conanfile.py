from conan import ConanFile


class ParserDataTamerConan(ConanFile):
    name = "parser_data_tamer"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "plotjuggler_core/[>=0.4.1 <0.5.0]",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
    )
    default_options = {"*:shared": False}
