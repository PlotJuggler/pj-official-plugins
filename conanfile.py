import os

from conan import ConanFile

# Single source of truth for the plotjuggler_sdk version: the SDK_VERSION file at the
# repo root, read live so the pin lives in exactly one place. Edit it with
# scripts/bump_core_version.py.
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
        "nanoarrow/0.7.0",
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
        "liblsl/1.16.2",
        # data_load_mp4 / data_load_lerobot (via common/pj_video_demux).
        "ffmpeg/8.1",
        "kissfft/131.1.0",
        "lua/5.4.6",
        "sol2/3.5.0",
        # libcurl powers anomaly_runner's notification sinks. (pj_scripting_core — the
        # Luau engine — is NOT required here: the GUI Anomaly Detector is host-driven /
        # Luau-free, and the headless runner is built separately via its own recipe with
        # -DPJ_BUILD_ANOMALY_RUNNER=ON.)
        "libcurl/8.10.1",
        # Pin libsodium to 1.0.20: 1.0.21 has broken ARM NEON code that fails with
        # GCC on aarch64.
        "libsodium/1.0.20",
        "pybind11/2.13.6",
        "cpython/3.12.7",
        # data_load_mf4: mdflib (vendored via CPM) links zlib + expat, provided
        # here from Conan. zlib/1.3.1 matches arrow/23.0.1's pin (no conflict).
        "zlib/1.3.1",
        "expat/2.6.4",
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
        "arrow/*:with_lz4": True,
        "arrow/*:with_zstd": True,
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
        # FFmpeg (data_load_mp4 / data_load_lerobot): those plugins only OPEN the container to read metadata
        # (creation_time, duration, codec name). It does not decode any frame
        # — decoding happens later in the host via FileVideoSource. Keep the
        # build lean and LGPL-clean: no encoders/muxers/devices/audio/codecs,
        # only the mov demuxer + file protocol + codec-id-to-name table.
        "ffmpeg/*:avcodec": True,
        "ffmpeg/*:avformat": True,
        "ffmpeg/*:swscale": False,
        "ffmpeg/*:swresample": False,
        "ffmpeg/*:avfilter": False,
        "ffmpeg/*:avdevice": False,
        "ffmpeg/*:postproc": False,
        "ffmpeg/*:with_programs": False,
        "ffmpeg/*:with_zlib": True,
        "ffmpeg/*:with_bzip2": False,
        "ffmpeg/*:with_lzma": False,
        "ffmpeg/*:with_libiconv": False,
        "ffmpeg/*:with_freetype": False,
        "ffmpeg/*:with_openjpeg": False,
        "ffmpeg/*:with_openh264": False,
        "ffmpeg/*:with_opus": False,
        "ffmpeg/*:with_vorbis": False,
        "ffmpeg/*:with_libx264": False,
        "ffmpeg/*:with_libx265": False,
        "ffmpeg/*:with_libvpx": False,
        "ffmpeg/*:with_libmp3lame": False,
        "ffmpeg/*:with_libfdk_aac": False,
        "ffmpeg/*:with_libwebp": False,
        "ffmpeg/*:with_ssl": False,
        "ffmpeg/*:with_libalsa": False,
        "ffmpeg/*:with_pulse": False,
        "ffmpeg/*:with_vaapi": False,
        "ffmpeg/*:with_vdpau": False,
        "ffmpeg/*:with_xcb": False,
        # The conan recipe rejects display/extra deps unless avdevice is on
        # (e.g. with_xlib requires avdevice). We have avdevice=False, so all
        # of these must be explicitly disabled.
        "ffmpeg/*:with_xlib": False,
        "ffmpeg/*:with_libdrm": False,
        "ffmpeg/*:with_libxml2": False,
        "ffmpeg/*:with_fontconfig": False,
        "ffmpeg/*:with_fribidi": False,
        "ffmpeg/*:with_harfbuzz": False,
        "ffmpeg/*:with_libjxl": False,
        "ffmpeg/*:with_openapv": False,
        "ffmpeg/*:with_zeromq": False,
        "ffmpeg/*:with_sdl": False,
        "ffmpeg/*:with_appkit": False,
        "ffmpeg/*:with_audiotoolbox": False,
        "ffmpeg/*:with_avfoundation": False,
        "ffmpeg/*:with_coreimage": False,
        "ffmpeg/*:with_videotoolbox": False,
        "ffmpeg/*:with_libsvtav1": False,
        "ffmpeg/*:with_libaom": False,
        "ffmpeg/*:with_libdav1d": False,
        "ffmpeg/*:disable_all_encoders": True,
        "ffmpeg/*:disable_all_muxers": True,
        "ffmpeg/*:disable_all_decoders": True,
        "ffmpeg/*:disable_all_demuxers": True,
        "ffmpeg/*:enable_demuxers": "mov",
        "ffmpeg/*:disable_all_parsers": True,
        "ffmpeg/*:disable_all_bitstream_filters": True,
        "ffmpeg/*:disable_all_protocols": True,
        "ffmpeg/*:enable_protocols": "file",
    }

    def requirements(self):
        # liblsl depends on pugixml/1.13, whose CMakeLists declares a pre-3.5
        # cmake_minimum_required that CMake 4.x refuses; 1.15 is API-compatible.
        self.requires("pugixml/1.15", override=True)
        # liblsl pins boost/1.81.0 exactly while Arrow's thrift wants [>=1.85 <=1.90].
        # Settle on thrift's choice (the Flight stack is the expensive one); liblsl
        # only needs Boost headers and builds fine against 1.90.
        self.requires("boost/1.90.0", override=True)

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
