import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class DataLoadLerobotConan(ConanFile):
    name = "data_load_lerobot"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "arrow/23.0.1",
        "ffmpeg/8.1",
    )
    default_options = {
        "*:shared": False,
        "arrow/*:parquet": True,
        "arrow/*:with_snappy": True,
        "boost/*:without_test": True,
        "boost/*:without_cobalt": True,
        # FFmpeg is used (via pj_video_demux) ONLY to walk the MP4 container
        # index — no decode, no bitstream filters. Same lean, LGPL-clean profile
        # as data_load_mp4: mov demuxer + file protocol only, everything else off.
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
