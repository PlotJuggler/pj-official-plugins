#!/usr/bin/env python3
"""
Foxglove WebSocket image server — emits synthetic JPEG frames.

Publishes sensor_msgs/CompressedImage on /camera/image/compressed at 10 Hz.
Each frame is a 320x240 RGB image with an animated colour gradient and a
bouncing ball so it's easy to see motion in the viewer.

Usage:
    pip install websockets pillow numpy
    python3 foxglove_image_server.py [--port PORT] [--host HOST]
"""

import argparse
import asyncio
import io
import json
import math
import struct
import time

import numpy as np
import websockets
from PIL import Image, ImageDraw

PORT = 8765
HOST = "localhost"

MESSAGE_DATA_OPCODE = 0x01

# Full flattened schema for sensor_msgs/msg/CompressedImage including all
# nested type definitions — rosx_introspection requires the complete dependency
# tree to be present in a single schema string.
COMPRESSED_IMAGE_SCHEMA = """\
std_msgs/Header header
string format
uint8[] data
================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id
================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec
"""

CHANNELS = [
    {
        "id": 1,
        "topic": "/camera/image/compressed",
        "encoding": "cdr",
        "schemaName": "sensor_msgs/msg/CompressedImage",
        "schema": COMPRESSED_IMAGE_SCHEMA,
        "schemaEncoding": "ros2msg",
    },
]

CHANNEL_BY_ID = {ch["id"]: ch for ch in CHANNELS}

WIDTH, HEIGHT = 320, 240


def make_frame(t: float) -> bytes:
    """Generate a 320x240 JPEG with an animated gradient and bouncing ball."""
    # Background: colour gradient that shifts over time
    img = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)

    r_shift = int(127 + 127 * math.sin(t * 0.5))
    g_shift = int(127 + 127 * math.sin(t * 0.7 + 2.0))
    b_shift = int(127 + 127 * math.sin(t * 0.3 + 4.0))

    for y in range(HEIGHT):
        for x in range(WIDTH):
            img[y, x, 0] = (r_shift + x) % 256
            img[y, x, 1] = (g_shift + y) % 256
            img[y, x, 2] = b_shift

    pil_img = Image.fromarray(img, "RGB")

    # Bouncing ball
    bx = int(WIDTH  * 0.5 + (WIDTH  * 0.4) * math.sin(t * 1.2))
    by = int(HEIGHT * 0.5 + (HEIGHT * 0.4) * math.cos(t * 0.9))
    draw = ImageDraw.Draw(pil_img)
    draw.ellipse([bx - 15, by - 15, bx + 15, by + 15], fill=(255, 255, 255))

    # Timestamp text
    draw.rectangle([0, 0, 160, 20], fill=(0, 0, 0))
    draw.text((4, 4), f"t={t:.2f}s", fill=(255, 255, 0))

    buf = io.BytesIO()
    pil_img.save(buf, format="JPEG", quality=70)
    return buf.getvalue()


def encode_compressed_image_cdr(t: float) -> bytes:
    """CDR-encode sensor_msgs/msg/CompressedImage."""
    jpeg_bytes = make_frame(t)
    ns = int(t * 1e9)
    sec = ns // 1_000_000_000
    nanosec = ns % 1_000_000_000
    frame_id = b"camera"

    buf = bytearray()
    # CDR encapsulation header: little-endian
    buf += b"\x00\x01\x00\x00"

    # Header.stamp.sec  (uint32)
    buf += struct.pack("<I", sec)
    # Header.stamp.nanosec (uint32)
    buf += struct.pack("<I", nanosec)
    # Header.frame_id (string: uint32 length + bytes + null terminator)
    buf += struct.pack("<I", len(frame_id) + 1)
    buf += frame_id + b"\x00"
    # CDR alignment to 4 bytes after the string
    while len(buf) % 4 != 0:
        buf += b"\x00"

    # format (string: "jpeg")
    fmt = b"jpeg"
    buf += struct.pack("<I", len(fmt) + 1)
    buf += fmt + b"\x00"
    while len(buf) % 4 != 0:
        buf += b"\x00"

    # data (sequence<uint8>: uint32 length + raw bytes)
    buf += struct.pack("<I", len(jpeg_bytes))
    buf += jpeg_bytes

    return bytes(buf)


def build_binary_frame(subscription_id: int, log_time_ns: int, cdr: bytes) -> bytes:
    """[opcode:1][subscription_id:4 LE][log_time_ns:8 LE][cdr_payload]"""
    return struct.pack("<BIQ", MESSAGE_DATA_OPCODE, subscription_id, log_time_ns) + cdr


class FoxgloveImageServer:
    def __init__(self):
        self.clients: dict = {}
        self._readvertise_tasks: dict = {}

    async def handler(self, websocket):
        client_id = id(websocket)
        self.clients[websocket] = {}
        print(f"[+] Client connected: {client_id}")

        await websocket.send(json.dumps({"op": "advertise", "channels": CHANNELS}))
        print(f"    advertise → {[ch['topic'] for ch in CHANNELS]}")

        task = asyncio.create_task(self._readvertise_until_subscribed(websocket))
        self._readvertise_tasks[websocket] = task

        try:
            async for message in websocket:
                await self.on_message(websocket, message)
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            task.cancel()
            self._readvertise_tasks.pop(websocket, None)
            del self.clients[websocket]
            print(f"[-] Client disconnected: {client_id}")

    async def _readvertise_until_subscribed(self, websocket):
        try:
            while True:
                await asyncio.sleep(2.0)
                if not self.clients.get(websocket):
                    await websocket.send(json.dumps({"op": "advertise", "channels": CHANNELS}))
                    print("    re-advertise → waiting for subscribe")
                else:
                    break
        except (asyncio.CancelledError, websockets.exceptions.ConnectionClosed):
            pass

    async def on_message(self, websocket, message):
        if isinstance(message, bytes):
            return
        try:
            msg = json.loads(message)
        except json.JSONDecodeError:
            return

        op = msg.get("op", "")
        if op == "subscribe":
            for sub in msg.get("subscriptions", []):
                sub_id = sub.get("id")
                channel_id = sub.get("channelId")
                if sub_id is not None and channel_id in CHANNEL_BY_ID:
                    self.clients[websocket][channel_id] = sub_id
            print(f"    subscribe → channel ids {list(self.clients[websocket].keys())}")
        elif op == "unsubscribe":
            for sub_id in msg.get("subscriptionIds", []):
                self.clients[websocket] = {
                    ch_id: s_id
                    for ch_id, s_id in self.clients[websocket].items()
                    if s_id != sub_id
                }

    async def emit_loop(self):
        """Send JPEG frames at 10 Hz."""
        frame_interval = 0.1
        t = 0.0
        while True:
            await asyncio.sleep(frame_interval)
            t += frame_interval

            if not self.clients:
                continue

            log_time_ns = int(time.time() * 1e9)
            cdr = encode_compressed_image_cdr(t)

            for websocket, subscriptions in list(self.clients.items()):
                if not subscriptions:
                    continue
                for channel_id, sub_id in list(subscriptions.items()):
                    frame = build_binary_frame(sub_id, log_time_ns, cdr)
                    try:
                        await websocket.send(frame)
                    except websockets.exceptions.ConnectionClosed:
                        pass

            print(f"    → frame t={t:.1f}s  size={len(cdr)} bytes", end="\r")


async def main(host: str, port: int):
    server = FoxgloveImageServer()
    print(f"Foxglove image server listening on ws://{host}:{port}")
    print(f"Topic: /camera/image/compressed (sensor_msgs/msg/CompressedImage, JPEG, 10 Hz)")
    print("Ctrl+C to stop\n")

    async with websockets.serve(
        server.handler,
        host,
        port,
        subprotocols=["foxglove.sdk.v1"],
    ):
        await server.emit_loop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Foxglove image server — synthetic JPEG frames")
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()

    try:
        asyncio.run(main(args.host, args.port))
    except KeyboardInterrupt:
        print("\nStopped.")
