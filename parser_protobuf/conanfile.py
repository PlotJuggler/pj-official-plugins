from conan import ConanFile


class ParserProtobufConan(ConanFile):
    name = "parser_protobuf"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "protobuf/6.33.5",
    )
    default_options = {"*:shared": False}
