from conan import ConanFile


class DataLoadLerobotConan(ConanFile):
    name = "data_load_lerobot"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "arrow/23.0.1",
    )
    default_options = {
        "*:shared": False,
        "arrow/*:parquet": True,
        "arrow/*:with_snappy": True,
        "arrow/*:with_zstd": True,
        "boost/*:without_test": True,
        "boost/*:without_cobalt": True,
    }
