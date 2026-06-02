import os
from conan import ConanFile


_SDK_VERSION = (
    open(os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "SDK_VERSION"))
    .read()
    .strip()
)


class DataStreamWebrtcConan(ConanFile):
    name = "data_stream_webrtc"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        f"plotjuggler_sdk/{_SDK_VERSION}",
        "gtest/1.17.0",
        "nlohmann_json/3.12.0",
        "libdatachannel/0.24.0",
    )
    default_options = {
        "*:shared": False,
        # libdatachannel media/SRTP path is always compiled in (no NO_MEDIA
        # option in the recipe). These pin the transport stack we rely on:
        #   * with_websocket=True  -> rtc::WebSocket for signaling
        #   * with_nice=False      -> use the bundled libjuice ICE agent
        #   * with_ssl=openssl     -> DTLS/SRTP backend
        "libdatachannel/*:with_websocket": True,
        "libdatachannel/*:with_nice": False,
        "libdatachannel/*:with_ssl": "openssl",
    }
