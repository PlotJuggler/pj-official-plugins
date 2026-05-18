from conan import ConanFile


class DataLoadLerobotConan(ConanFile):
    name = "data_load_lerobot"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "arrow/23.0.1",
        "ffmpeg/8.1",
    )
    default_options = {
        "*:shared": False,
        "arrow/*:parquet": True,
        "arrow/*:with_snappy": True,
        "arrow/*:with_zstd": True,
        "boost/*:without_test": True,
        "boost/*:without_cobalt": True,
        # FFmpeg: the plugin DECODES each camera frame (LeRobot mp4 may be
        # AV1/H.264/H.265/…) and re-encodes it to JPEG so PlotJuggler's
        # built-in kImage→JPEG pipeline can display it (no parser needed, and
        # PJ4's own ffmpeg codec set becomes irrelevant for our images).
        # LGPL-clean: dav1d (BSD) for AV1 decode, built-in mjpeg encoder, no
        # GPL/nonfree codecs, no muxers/devices.
        "ffmpeg/*:avcodec": True,
        "ffmpeg/*:avformat": True,
        "ffmpeg/*:swscale": True,
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
        # of these must be explicitly disabled (mirrors the PJ4 monorepo
        # ffmpeg stanza).
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
        "ffmpeg/*:with_libdav1d": True,
        "ffmpeg/*:disable_all_encoders": True,
        "ffmpeg/*:enable_encoders": "mjpeg",
        "ffmpeg/*:disable_all_muxers": True,
        "ffmpeg/*:disable_all_decoders": True,
        # libdav1d = the SOFTWARE AV1 decoder; the bare "av1" decoder is a
        # HW-only stub and fails headless. disable_all_decoders=True means
        # libdav1d must be in this allowlist explicitly.
        "ffmpeg/*:enable_decoders": "h264,hevc,mjpeg,libdav1d,vp9,vp8,mpeg4",
        "ffmpeg/*:disable_all_demuxers": True,
        "ffmpeg/*:enable_demuxers": "mov,matroska,avi",
        "ffmpeg/*:disable_all_parsers": True,
        "ffmpeg/*:enable_parsers": "h264,hevc,mjpeg,av1,vp9,vp8,mpeg4video",
        "ffmpeg/*:disable_all_bitstream_filters": True,
        "ffmpeg/*:enable_bitstream_filters": "h264_mp4toannexb,hevc_mp4toannexb",
        "ffmpeg/*:disable_all_protocols": True,
        "ffmpeg/*:enable_protocols": "file",
    }
