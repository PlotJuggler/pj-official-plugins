import os

from conan import ConanFile

# The only plotjuggler_sdk pin: the SDK_VERSION file at the repo root. Edit it with
# scripts/bump_core_version.py.
_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "SDK_VERSION"))
    .read()
    .strip()
)


class PjOfficialPluginsConan(ConanFile):
    """Full-repository dependency superset, used by `build.sh` with no argument and by full CI builds.

    Per-plugin builds use each plugin's own conanfile.py.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    requires = (
        "nlohmann_json/3.12.0",
        "arrow/26.0.1",
        "fmt/12.1.0",
        "paho-mqtt-cpp/1.6.0",
        "cppzmq/4.11.0",
        "protobuf/6.33.5",
        # Must equal arrow/26.0.1's own lz4 pin (it hard-requires lz4/1.9.4 once with_lz4=True, which
        # the Linux Flight build enables); a newer lz4 conflicts and re-splits Arrow into a second
        # build. Bump only in lockstep with arrow. data_load_mcap's own recipe pins lz4/1.10.0.
        "lz4/1.9.4",
        "zstd/1.5.7",
        "date/3.0.4",
        "gtest/1.17.0",
        "ixwebsocket/12.0.0",
        "libdatachannel/0.24.0",
        "asio/1.28.2",
        "liblsl/1.16.2",
        # data_load_mp4 / data_load_lerobot (via common/pj_video_demux).
        "ffmpeg/8.1.2",
        "pocketfft/0.0.0.cci.20240801",
        "lua/5.4.6",
        "sol2/3.5.0",
        # anomaly_runner's notification sinks. pj_scripting_core is not required here: the runner
        # builds via its own recipe.
        "libcurl/8.23.0",
        # Not 1.0.21: its ARM NEON code fails with GCC on aarch64 (fixed in 1.0.22).
        "libsodium/1.0.22",
        # data_load_mf4: mdflib (via CPM) links zlib + expat from Conan; zlib/1.3.2 is inside
        # arrow/26.0.1's zlib range.
        "zlib/1.3.2",
        "expat/2.10.0",
        f"plotjuggler_sdk/{_SDK_VERSION}",
    )

    # Puts the Conan protoc on PATH so parser_protobuf's protobuf_generate() does not pick the system
    # /usr/bin/protoc (3.21), whose generated C++ is ABI-incompatible with the libprotobuf 6.x headers.
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
        "pocketfft/*:multithreading": False,
        # WebRTC streaming client (data_stream_webrtc): media/SRTP/WebSocket on,
        # libnice off (use the bundled libjuice ICE agent).
        "libdatachannel/*:with_websocket": True,
        "libdatachannel/*:with_nice": False,
        "libdatachannel/*:with_ssl": "openssl",
        # FFmpeg (data_load_mp4 / data_load_lerobot) only opens the container to read metadata
        # (creation_time, duration, codec name); the host decodes frames. Kept lean and LGPL-clean:
        # only the mov demuxer, file protocol and codec-id-to-name table.
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
        # The recipe rejects display/extra deps unless avdevice is on (e.g. with_xlib), so with
        # avdevice=False all of these must be disabled explicitly.
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
        # toolbox_mosaico's Flight client needs a real Arrow Flight target or its CMakeLists
        # self-skips; enabling Flight on the shared arrow lets it build inside the aggregate in one
        # pass. A strict superset of the lean options, so other Arrow consumers are unaffected.
        # macOS stays lean (Flight off): the Flight/gRPC stack is not validated there.
        if self.settings.os in ("Linux", "Windows"):
            self.options["arrow"].with_flight_rpc = True
            self.options["arrow"].with_grpc = True
            self.options["arrow"].with_protobuf = True
            self.options["arrow"].with_re2 = True
            self.options["arrow"].with_thrift = True
