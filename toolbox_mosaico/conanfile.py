from conan import ConanFile


class ToolboxMosaicoConan(ConanFile):
    name = "toolbox_mosaico"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        # Mosaico needs SDK additions that landed in the 0.3.1 patch, so the
        # floor is 0.3.1 (the rest of the repo uses the looser [~0.3] = >=0.3.0).
        "plotjuggler_core/[>=0.3.1 <0.4.0]",
        "nlohmann_json/3.12.0",
        "arrow/23.0.1",
        "lua/5.4.6",
        "sol2/3.5.0",
        # fmt required transitively by plotjuggler_core's pj_plugins
        "fmt/12.1.0",
    )
    default_options = {
        "*:shared": False,
        "arrow/*:parquet": True,
        "arrow/*:with_snappy": True,
        "arrow/*:with_flight_rpc": True,  # required by Mosaico SDK (Arrow Flight client)
        "arrow/*:with_grpc": True,
        "arrow/*:with_protobuf": True,
        "arrow/*:with_re2": True,
        "arrow/*:with_thrift": True,
        # The Mosaico server sends RecordBatches compressed with LZ4.
        # Without these, pullTopic fails with "NotImplemented: Support
        # for codec 'lz4' not built".
        "arrow/*:with_lz4": True,
        "arrow/*:with_zstd": True,
        "lua/*:compile_as_cpp": True,
        "boost/*:without_test": True,
        "boost/*:without_cobalt": True,
    }
