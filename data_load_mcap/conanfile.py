from conan import ConanFile


class DataLoadMcapConan(ConanFile):
    name = "data_load_mcap"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # mcap headers are vendored in contrib/mcap/ (from the parallel-reader
    # fork). We still need lz4 and zstd for chunk decompression — previously
    # transitively from the mcap/ package, now declared directly here.
    requires = (
        "plotjuggler_core/[~0.5]",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "lz4/1.10.0",
        "zstd/1.5.7",
    )
    default_options = {"*:shared": False}
