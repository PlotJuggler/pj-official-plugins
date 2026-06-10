import os

from conan import ConanFile

# Single source of truth for the plotjuggler_sdk version: the SDK_VERSION file at the
# repo root (read live), shared by the root recipe and every plugin recipe. Edit it with
# scripts/bump_core_version.py. Mosaico's cursor-aware query-assist needs the caret dialog
# SDK (onCodeChangedWithCursor / codeChanged(code,cursor) / codeCursor) that landed in
# 0.5.1, which the repo-wide pin (now 0.6.0) comfortably satisfies.
_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class ToolboxMosaicoConan(ConanFile):
    name = "toolbox_mosaico"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "arrow/23.0.1",
        "lua/5.4.6",
        "sol2/3.5.0",
        # fmt required transitively by plotjuggler_sdk's pj_plugins
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
