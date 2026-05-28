#!/usr/bin/env python3
"""
PJ Bridge mixed test server — scalars + JPEG image frames.

Publishes at 10 Hz:
  /test/sine       (json)  — float64 sine wave
  /test/cosine     (json)  — float64 cosine wave
  /test/imu        (json)  — nested object: {accel:{x,y,z}, gyro:{x,y,z}}
  /camera/image    (cdr)   — sensor_msgs/msg/CompressedImage JPEG 320x240

Usage:
    pip install websockets zstandard pillow numpy
    python3 pj_bridge_mixed_server.py [--port PORT] [--host HOST]
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
import zstandard as zstd
from PIL import Image, ImageDraw

PORT = 9871
HOST = "localhost"
MAGIC = 0x42524A50  # "PJRB" LE

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

TOPICS = [
    {"name": "/test/sine",    "type": "Float64",  "schema_name": "Float64",  "encoding": "json", "definition": ""},
    {"name": "/test/cosine",  "type": "Float64",  "schema_name": "Float64",  "encoding": "json", "definition": ""},
    {"name": "/test/imu",     "type": "Imu",      "schema_name": "Imu",      "encoding": "json", "definition": ""},
    {
        "name": "/camera/image",
        "type": "sensor_msgs/msg/CompressedImage",
        "schema_name": "sensor_msgs/msg/CompressedImage",
        "encoding": "cdr",
        "definition": COMPRESSED_IMAGE_SCHEMA,
    },
]

WIDTH, HEIGHT = 320, 240


# ---------------------------------------------------------------------------
# Encoding helpers
# ---------------------------------------------------------------------------

def make_jpeg(t: float) -> bytes:
    img = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    r = int(127 + 127 * math.sin(t * 0.5))
    g = int(127 + 127 * math.sin(t * 0.7 + 2.0))
    for y in range(HEIGHT):
        for x in range(WIDTH):
            img[y, x, 0] = (r + x) % 256
            img[y, x, 1] = (g + y) % 256
            img[y, x, 2] = 120
    pil = Image.fromarray(img, "RGB")
    bx = int(WIDTH  * 0.5 + (WIDTH  * 0.4) * math.sin(t * 1.2))
    by = int(HEIGHT * 0.5 + (HEIGHT * 0.4) * math.cos(t * 0.9))
    draw = ImageDraw.Draw(pil)
    draw.ellipse([bx - 15, by - 15, bx + 15, by + 15], fill=(255, 255, 255))
    draw.rectangle([0, 0, 120, 16], fill=(0, 0, 0))
    draw.text((2, 2), f"t={t:.1f}s", fill=(255, 220, 0))
    buf = io.BytesIO()
    pil.save(buf, format="JPEG", quality=65)
    return buf.getvalue()


def encode_compressed_image_cdr(t: float) -> bytes:
    jpeg = make_jpeg(t)
    ns = int(t * 1e9)
    sec, nanosec = ns // 1_000_000_000, ns % 1_000_000_000
    frame_id = b"camera"

    buf = bytearray(b"\x00\x01\x00\x00")  # CDR LE header
    buf += struct.pack("<II", sec, nanosec)
    buf += struct.pack("<I", len(frame_id) + 1) + frame_id + b"\x00"
    while len(buf) % 4:
        buf += b"\x00"
    fmt = b"jpeg"
    buf += struct.pack("<I", len(fmt) + 1) + fmt + b"\x00"
    while len(buf) % 4:
        buf += b"\x00"
    buf += struct.pack("<I", len(jpeg)) + jpeg
    return bytes(buf)


def build_binary_frame(messages: list) -> bytes:
    payload = bytearray()
    for topic, ts_ns, cdr in messages:
        tb = topic.encode()
        payload += struct.pack("<H", len(tb)) + tb
        payload += struct.pack("<q", ts_ns)
        payload += struct.pack("<I", len(cdr)) + cdr

    compressed = zstd.ZstdCompressor().compress(bytes(payload))
    return struct.pack("<IIII", MAGIC, len(messages), len(payload), 0) + compressed


# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------

class PjBridgeMixedServer:
    def __init__(self):
        self.clients: dict = {}

    async def handler(self, websocket):
        self.clients[websocket] = {"paused": False, "subscribed_topics": set()}
        print(f"[+] Client connected")
        try:
            async for message in websocket:
                if isinstance(message, bytes):
                    continue
                await self._on_text(websocket, message)
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            self.clients.pop(websocket, None)
            print(f"[-] Client disconnected")

    async def _on_text(self, websocket, message):
        try:
            msg = json.loads(message)
        except json.JSONDecodeError:
            return

        cmd = msg.get("command", "")

        if cmd == "get_topics":
            await websocket.send(json.dumps({"status": "success", "topics": TOPICS}))
            print(f"    get_topics → {[t['name'] for t in TOPICS]}")

        elif cmd == "subscribe":
            requested = set(msg.get("topics", []))
            self.clients[websocket]["subscribed_topics"] = requested

            schemas = {}
            for t in TOPICS:
                if t["name"] in requested:
                    schemas[t["name"]] = {
                        "encoding":   t["encoding"],
                        "definition": t["definition"],
                        "schema_name": t["schema_name"],
                    }
            await websocket.send(json.dumps({"status": "success", "schemas": schemas}))
            print(f"    subscribe → {sorted(requested)}")

        elif cmd == "pause":
            self.clients[websocket]["paused"] = True
        elif cmd == "resume":
            self.clients[websocket]["paused"] = False

    async def emit_loop(self):
        t = 0.0
        img_tick = 0
        while True:
            await asyncio.sleep(0.1)
            t += 0.1
            img_tick += 1

            if not self.clients:
                continue

            ts_ns = int(time.time() * 1e9)

            scalar_payloads = {
                "/test/sine":   json.dumps({"value": math.sin(t)}).encode(),
                "/test/cosine": json.dumps({"value": math.cos(t)}).encode(),
                "/test/imu":    json.dumps({
                    "accel": {"x": round(math.sin(t*1.1), 4),
                              "y": round(math.cos(t*0.9), 4),
                              "z": round(9.81 + 0.05*math.sin(t*3), 4)},
                    "gyro":  {"x": round(math.sin(t*2)*0.1, 4),
                              "y": round(math.cos(t*1.5)*0.1, 4),
                              "z": round(0.01*math.sin(t*4), 4)},
                }).encode(),
            }
            image_cdr = encode_compressed_image_cdr(t) if img_tick % 1 == 0 else None

            for websocket, state in list(self.clients.items()):
                if state["paused"]:
                    continue
                subscribed = state["subscribed_topics"]
                if not subscribed:
                    continue

                messages = []
                for topic, cdr in scalar_payloads.items():
                    if topic in subscribed:
                        messages.append((topic, ts_ns, cdr))
                if image_cdr and "/camera/image" in subscribed:
                    messages.append(("/camera/image", ts_ns, image_cdr))

                if messages:
                    frame = build_binary_frame(messages)
                    try:
                        await websocket.send(frame)
                        topics = [m[0] for m in messages]
                        print(f"    → {len(frame)}b  {topics}", end="\r")
                    except websockets.exceptions.ConnectionClosed:
                        pass


async def main(host: str, port: int):
    server = PjBridgeMixedServer()
    print(f"PJ Bridge mixed server listening on ws://{host}:{port}")
    print(f"Scalars: /test/sine  /test/cosine  /test/imu  (json, 10 Hz)")
    print(f"Image:   /camera/image  (sensor_msgs/CompressedImage CDR JPEG, 10 Hz)")
    print("Ctrl+C to stop\n")

    async with websockets.serve(server.handler, host, port):
        await server.emit_loop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()
    try:
        asyncio.run(main(args.host, args.port))
    except KeyboardInterrupt:
        print("\nStopped.")
