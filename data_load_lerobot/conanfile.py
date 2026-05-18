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
        # FFmpeg: the plugin only DEMUXES mp4 and runs the annex-b bitstream
        # filter — decoding happens later inside pj_scene2D. Keep this lean and
        # LGPL-clean (no encoders/muxers/devices, no GPL/nonfree codecs).
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
        "ffmpeg/*:with_libsvtav1": False,
        "ffmpeg/*:with_libaom": False,
        "ffmpeg/*:with_libdav1d": False,
        "ffmpeg/*:disable_all_encoders": True,
        "ffmpeg/*:disable_all_muxers": True,
        "ffmpeg/*:disable_all_decoders": True,
        "ffmpeg/*:disable_all_demuxers": True,
        "ffmpeg/*:enable_demuxers": "mov,matroska,avi",
        "ffmpeg/*:disable_all_parsers": True,
        "ffmpeg/*:enable_parsers": "h264,hevc,mjpeg",
        "ffmpeg/*:disable_all_bitstream_filters": True,
        "ffmpeg/*:enable_bitstream_filters": "h264_mp4toannexb,hevc_mp4toannexb",
        "ffmpeg/*:disable_all_protocols": True,
        "ffmpeg/*:enable_protocols": "file",
    }
