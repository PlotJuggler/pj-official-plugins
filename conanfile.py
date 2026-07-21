import os

from conan import ConanFile

# Single source of truth for the plotjuggler_sdk version: the SDK_VERSION file at the
# repo root, read live so the pin lives in exactly one place. Edit it with
# scripts/bump_core_version.py (which also moves the extern/plotjuggler_core submodule).
_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "SDK_VERSION"))
    .read()
    .strip()
)


class PjOfficialPluginsConan(ConanFile):
    """Full-repository dependency superset (every plugin's third-party deps).

    Used by `build.sh` with no argument and by the scheduled/manual full builds.
    Per-plugin builds use each plugin's own conanfile.py instead.
    On Linux the aggregate builds Arrow with the full Flight stack (see configure())
    so toolbox_mosaico builds there too, not only via its own per-plugin recipe.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    requires = (
        "nlohmann_json/3.12.0",
        "arrow/23.0.1",
        "fmt/12.1.0",
        "paho-mqtt-cpp/1.5.3",
        "cppzmq/4.11.0",
        "protobuf/6.33.5",
        # Pinned to lz4/1.9.4 to match arrow/23.0.1's exact lz4 dependency: arrow's
        # recipe hard-requires lz4/1.9.4 once with_lz4=True, which configure() turns
        # on for the Linux Flight build. They MUST stay equal there — a newer lz4
        # collides with arrow's pin (Conan version conflict) and re-splits Arrow into
        # a second build. On non-Linux (Flight off) arrow pulls no lz4, so this pin is
        # standalone for the lean plugins (e.g. data_load_mcap). data_load_mcap's OWN
        # per-plugin recipe still pins lz4/1.10.0; that artifact is built separately
        # and is unaffected. Bump only in lockstep with arrow's lz4 pin.
        "lz4/1.9.4",
        "zstd/1.5.7",
        "date/3.0.4",
        "gtest/1.17.0",
        "ixwebsocket/11.4.6",
        "libdatachannel/0.24.0",
        "asio/1.28.2",
        # NOTE: liblsl (data_stream_lsl) is intentionally NOT here. It requires
        # boost/[>=1.85], which conflicts with the boost/1.81.0 that Arrow's
        # Flight stack pins via thrift in this aggregate. data_stream_lsl builds
        # per-plugin against its own conanfile.py (like data_load_lerobot/mp4).
        "kissfft/131.1.0",
        "lua/5.4.6",
        "sol2/3.5.0",
        # Pin libsodium to 1.0.20: 1.0.21 has broken ARM NEON code that fails with
        # GCC on aarch64.
        "libsodium/1.0.20",
        "pybind11/2.13.6",
        "cpython/3.12.7",
        f"plotjuggler_sdk/{_SDK_VERSION}",
    )

    # Build-context protobuf so the Conan protoc (6.33.5) lands on the build
    # PATH and protobuf_generate()'s find_program(protoc) in parser_protobuf
    # resolves to it, NOT the system /usr/bin/protoc (3.21) — whose generated C++
    # is ABI-incompatible with the libprotobuf 6.x headers we link against.
    tool_requires = ("protobuf/6.33.5",)

    default_options = {
        "*:shared": False,
        "arrow/*:parquet": True,
        "arrow/*:with_snappy": True,
        # mimalloc uses initial-exec TLS, making every .so linking it require static
        # TLS and fail to dlopen once the process's static-TLS surplus is exhausted.
        "arrow/*:with_mimalloc": False,
        "boost/*:without_test": True,
        "boost/*:without_cobalt": True,
        "lua/*:compile_as_cpp": True,
        # cpython built static on Linux/macOS: libpython3.12.a links into the plugin
        # .so directly. Windows/MSVC disables static cpython >=3.10, so ci-windows.yml
        # overrides to shared=True via -o flag; the CMakeLists.txt then copies
        # python3XX.dll.
        "cpython/*:shared": False,
        # Tkinter requires X11 system libs (libx11-dev, libxcb-*-dev) which are not
        # available on CI runners. We do not need Tkinter for scripting.
        "cpython/*:with_tkinter": False,
        # WebRTC streaming client (data_stream_webrtc): media/SRTP/WebSocket on,
        # libnice off (use the bundled libjuice ICE agent).
        "libdatachannel/*:with_websocket": True,
        "libdatachannel/*:with_nice": False,
        "libdatachannel/*:with_ssl": "openssl",
    }

    def configure(self):
        # Enable Arrow's Flight stack on Linux AND Windows. toolbox_mosaico's Arrow
        # Flight client (the Mosaico server connection) needs a real Flight target or
        # its CMakeLists self-skips. Turning Flight on for the single shared
        # arrow/23.0.1 here is what lets toolbox_mosaico build inside the AGGREGATE
        # (./build.sh with no arg) in ONE pass, instead of needing a second,
        # standalone Arrow-with-Flight build. These options are a strict superset of
        # the lean parquet/snappy options, so the other Arrow consumers
        # (data_load_parquet/lerobot) are unaffected.
        #
        # Windows was validated to build the whole Flight/gRPC stack under MSVC
        # (VS2022 / msvc194), so it now builds mosaico too rather than self-skipping.
        # macOS stays lean (Flight off) until it is likewise validated.
        if self.settings.os in ("Linux", "Windows"):
            self.options["arrow"].with_flight_rpc = True
            self.options["arrow"].with_grpc = True
            self.options["arrow"].with_protobuf = True
            self.options["arrow"].with_re2 = True
            self.options["arrow"].with_thrift = True
            # The Mosaico server streams RecordBatches compressed with LZ4/ZSTD.
            self.options["arrow"].with_lz4 = True
            self.options["arrow"].with_zstd = True
