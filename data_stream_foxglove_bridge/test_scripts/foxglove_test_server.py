#!/usr/bin/env python3
"""
Foxglove WebSocket test server for data_stream_foxglove_bridge plugin.

Publishes 3 synthetic ROS2 topics in CDR format at 10 Hz:
  - /sensor/temperature  (std_msgs/Float64)
  - /sensor/position     (geometry_msgs/Point)
  - /sensor/status       (std_msgs/Int32)

Usage:
    python3 foxglove_test_server.py

Then connect pj_proto_app to localhost:8765 via Foxglove Bridge plugin.
"""

import asyncio
import math
import struct
import time

from foxglove_websocket import run_cancellable
from foxglove_websocket.server import FoxgloveServer, FoxgloveServerListener
from foxglove_websocket.types import ChannelId


# ---------------------------------------------------------------------------
# CDR encoding helpers
# ---------------------------------------------------------------------------

CDR_HEADER = b"\x00\x01\x00\x00"  # little-endian CDR header


def encode_float64(value: float) -> bytes:
    """std_msgs/Float64: float64 data"""
    return CDR_HEADER + struct.pack("<d", value)


def encode_int32(value: int) -> bytes:
    """std_msgs/Int32: int32 data"""
    return CDR_HEADER + struct.pack("<i", value)


def encode_point(x: float, y: float, z: float) -> bytes:
    """geometry_msgs/Point: float64 x, float64 y, float64 z"""
    return CDR_HEADER + struct.pack("<ddd", x, y, z)


# ---------------------------------------------------------------------------
# ROS2 message schemas (.msg format)
# ---------------------------------------------------------------------------

SCHEMA_FLOAT64 = "float64 data"
SCHEMA_INT32 = "int32 data"
SCHEMA_POINT = "float64 x\nfloat64 y\nfloat64 z"


# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------

class Listener(FoxgloveServerListener):
    def on_subscribe(self, server: FoxgloveServer, channel_id: ChannelId):
        print(f"  [+] Client subscribed to channel {channel_id}")

    def on_unsubscribe(self, server: FoxgloveServer, channel_id: ChannelId):
        print(f"  [-] Client unsubscribed from channel {channel_id}")


async def main():
    async with FoxgloveServer("0.0.0.0", 8765, "PlotJuggler test server") as server:
        server.set_listener(Listener())

        # Advertise channels
        ch_temperature = await server.add_channel({
            "topic": "/sensor/temperature",
            "encoding": "cdr",
            "schemaName": "std_msgs/Float64",
            "schema": SCHEMA_FLOAT64,
            "schemaEncoding": "ros2msg",
        })
        ch_position = await server.add_channel({
            "topic": "/sensor/position",
            "encoding": "cdr",
            "schemaName": "geometry_msgs/Point",
            "schema": SCHEMA_POINT,
            "schemaEncoding": "ros2msg",
        })
        ch_status = await server.add_channel({
            "topic": "/sensor/status",
            "encoding": "cdr",
            "schemaName": "std_msgs/Int32",
            "schema": SCHEMA_INT32,
            "schemaEncoding": "ros2msg",
        })

        print("Foxglove test server running on ws://localhost:8765")
        print("Topics:")
        print("  /sensor/temperature  (std_msgs/Float64)  — sine wave")
        print("  /sensor/position     (geometry_msgs/Point) — circular motion")
        print("  /sensor/status       (std_msgs/Int32)    — counter")
        print("\nConnect pj_proto_app → Start Stream → Foxglove Bridge → localhost:8765")
        print("Press Ctrl+C to stop.\n")

        t = 0.0
        counter = 0
        while True:
            now_ns = time.time_ns()

            temperature = 20.0 + 5.0 * math.sin(t)
            x = math.cos(t)
            y = math.sin(t)
            z = 0.5 * math.sin(2 * t)

            await server.send_message(ch_temperature, now_ns, encode_float64(temperature))
            await server.send_message(ch_position,    now_ns, encode_point(x, y, z))
            await server.send_message(ch_status,      now_ns, encode_int32(counter))

            t += 0.1
            counter += 1
            await asyncio.sleep(0.1)  # 10 Hz


if __name__ == "__main__":
    run_cancellable(main())
