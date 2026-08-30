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
        # mosaico_ingest_core's cache artifact is an MCAP container; the vendored
        # header-only mcap needs lz4 + zstd for chunk (de)compression. Arrow pulls
        # both in, but Conan hides a transitive dependency's headers, so without a
        # direct require mcap would compile against the host's /usr/include copies.
        # Versions match arrow/23.0.1's own pins (see data_load_mosaico_cache).
        "lz4/1.9.4",
        "zstd/1.5.7",
        # mosaico_ingest_core's SHA-256 wrapper includes OpenSSL EVP headers.
        # Arrow already resolves OpenSSL, but Conan hides transitive headers;
        # this direct require exposes them without selecting another version.
        "openssl/3.6.4",
        "lua/5.4.6",
        "sol2/3.5.0",
        # fmt required transitively by plotjuggler_sdk's pj_plugins
        "fmt/12.1.0",
    )
    default_options = {
        "*:shared": False,
        "arrow/*:parquet": True,
        "arrow/*:with_snappy": True,
        # mimalloc uses initial-exec TLS, making every .so linking it require static
        # TLS and fail to dlopen once the process's static-TLS surplus is exhausted.
        "arrow/*:with_mimalloc": False,
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
