import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class DataLoadMp4Conan(ConanFile):
    name = "data_load_mp4"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "fmt/12.1.0",
        "ffmpeg/8.1",
        "date/3.0.4",
    )
    default_options = {
        "*:shared": False,
        # FFmpeg: this plugin only OPENS the container to read metadata
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
