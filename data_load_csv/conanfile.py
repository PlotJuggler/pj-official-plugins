from conan import ConanFile


class DataLoadCsvConan(ConanFile):
    name = "data_load_csv"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "date/3.0.4",
    )
    default_options = {"*:shared": False}
