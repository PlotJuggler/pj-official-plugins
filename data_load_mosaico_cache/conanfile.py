import os

from conan import ConanFile

# Single source of truth for the plotjuggler_sdk version: the SDK_VERSION file at the
# repo root (read live), shared by the root recipe and every plugin recipe.
_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class DataLoadMosaicoCacheConan(ConanFile):
    name = "data_load_mosaico_cache"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # This loader replays cache artifacts and never talks to a Mosaico
    # server: it LINKS only plain Arrow (no Flight/gRPC targets). The Arrow
    # OPTION SET below still mirrors toolbox_mosaico's exactly, so both
    # recipes resolve to the same cached Arrow binary instead of building a
    # second Arrow from source per option-set.
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "arrow/23.0.1",
        # The cache artifact is an MCAP container; the vendored header-only
        # mcap needs lz4 + zstd for chunk (de)compression.
        "lz4/1.9.4",
        "zstd/1.5.7",
    )
    default_options = {
        "*:shared": False,
        "arrow/*:parquet": True,
        "arrow/*:with_snappy": True,
        # mimalloc uses initial-exec TLS, making every .so linking it require static
        # TLS and fail to dlopen once the process's static-TLS surplus is exhausted.
        "arrow/*:with_mimalloc": False,
        "arrow/*:with_flight_rpc": True,
        "arrow/*:with_grpc": True,
        "arrow/*:with_protobuf": True,
        "arrow/*:with_re2": True,
        "arrow/*:with_thrift": True,
        "arrow/*:with_lz4": True,
        "arrow/*:with_zstd": True,
        "boost/*:without_test": True,
        "boost/*:without_cobalt": True,
    }
