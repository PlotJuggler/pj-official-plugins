from conan import ConanFile


class DataLoadParquetConan(ConanFile):
    name = "data_load_parquet"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "date/3.0.4",
        "arrow/23.0.1",
    )
    default_options = {
        "*:shared": False,
        "arrow/*:parquet": True,
        "arrow/*:with_snappy": True,
        "boost/*:without_test": True,
        "boost/*:without_cobalt": True,
    }
