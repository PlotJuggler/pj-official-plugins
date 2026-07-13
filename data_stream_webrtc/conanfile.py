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
        "ixwebsocket/11.4.6",
    )
    default_options = {
        "*:shared": False,
        # libdatachannel media/SRTP path is always compiled in. These pin the
        # transport stack we rely on:
        #   * with_websocket=False -> no rtc::WebSocket (WHEP signaling is HTTP)
        #   * with_nice=False      -> use the bundled libjuice ICE agent
        #   * with_ssl=openssl     -> DTLS/SRTP backend
        "libdatachannel/*:with_websocket": False,
        "libdatachannel/*:with_nice": False,
        "libdatachannel/*:with_ssl": "openssl",
    }
