#!/usr/bin/env python3
"""
PJ Bridge mixed test server — scalars + JPEG image frames.

Publishes at 10 Hz:
  /test/sine       (json)  — float64 sine wave
  /test/cosine     (json)  — float64 cosine wave
  /test/imu        (json)  — nested object: {accel:{x,y,z}, gyro:{x,y,z}}
  /camera/image    (cdr)   — sensor_msgs/msg/CompressedImage JPEG 320x240

The JSON control plane mirrors the PRODUCTION server semantics
(github.com/PlotJuggler/plotjuggler_bridge): additive subscribe, unsubscribe
with a `removed` list, include_schemas on get_topics, subscribe_topic_updates
opt-in, and `id` + `protocol_version` echoed on every response. This server's
topic set is static, so it implements the topic_updates opt-in but never pushes
a topics_changed notification (there is nothing to announce).

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
PROTOCOL_VERSION = 1

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
    {"name": "/test/sine",   "type": "Float64", "encoding": "json", "definition": ""},
    {"name": "/test/cosine", "type": "Float64", "encoding": "json", "definition": ""},
    {"name": "/test/imu",    "type": "Imu",     "encoding": "json", "definition": ""},
    {
        "name": "/camera/image",
        "type": "sensor_msgs/msg/CompressedImage",
        # PARSER encoding (what production's schema_encoding() sends): the
        # definition below is a ROS .msg, so ros2msg — "cdr" resolves no parser.
        "encoding": "ros2msg",
        "definition": COMPRESSED_IMAGE_SCHEMA,
    },
]

WIDTH, HEIGHT = 320, 240


# ---------------------------------------------------------------------------
# Protocol helpers (identical across all three test servers)
# ---------------------------------------------------------------------------

def inject_response_fields(response: dict, request: dict) -> dict:
    """Attach protocol_version and echo the request's `id` when it is a string."""
    response["protocol_version"] = PROTOCOL_VERSION
    rid = request.get("id")
    if isinstance(rid, str):
        response["id"] = rid
    return response


def parse_topic_names(topics) -> list:
    """Extract topic names from a subscribe/unsubscribe `topics` array (mixed
    strings and {"name": ...} objects; rate limits ignored)."""
    names = []
    if isinstance(topics, list):
        for item in topics:
            if isinstance(item, str):
                names.append(item)
            elif isinstance(item, dict) and isinstance(item.get("name"), str):
                names.append(item["name"])
    return names


def topic_entry(topic: dict, include_schemas: bool) -> dict:
    """{name, type} by default; adds {encoding, definition} when requested."""
    entry = {"name": topic["name"], "type": topic["type"]}
    if topic.get("latched"):
        entry["latched"] = True  # explicit per-topic flag in the table (production badge)
    if include_schemas:
        entry["encoding"] = topic["encoding"]
        entry["definition"] = topic["definition"]
    return entry


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
        self.clients[websocket] = {
            "paused": False,
            "subscribed": set(),
            "topic_updates": False,
            "tu_include_schemas": False,
        }
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
        if not isinstance(msg, dict):
            return

        cmd = msg.get("command", "")
        state = self.clients[websocket]

        if cmd == "get_topics":
            include = bool(msg.get("include_schemas", False))
            resp = {"status": "success",
                    "topics": [topic_entry(t, include) for t in TOPICS]}
            await websocket.send(json.dumps(inject_response_fields(resp, msg)))
            print(f"    get_topics (include_schemas={include}) → {[t['name'] for t in TOPICS]}")

        elif cmd == "subscribe":
            await self._handle_subscribe(websocket, msg, state)

        elif cmd == "unsubscribe":
            requested = parse_topic_names(msg.get("topics", []))
            removed = [n for n in requested if n in state["subscribed"]]
            for n in removed:
                state["subscribed"].discard(n)
            resp = {"status": "success", "removed": removed}
            await websocket.send(json.dumps(inject_response_fields(resp, msg)))
            print(f"    unsubscribe → removed {removed}")

        elif cmd == "subscribe_topic_updates":
            state["topic_updates"] = True
            state["tu_include_schemas"] = bool(msg.get("include_schemas", False))
            resp = {"status": "ok", "topic_updates": True}
            await websocket.send(json.dumps(inject_response_fields(resp, msg)))

        elif cmd == "unsubscribe_topic_updates":
            state["topic_updates"] = False
            resp = {"status": "ok", "topic_updates": False}
            await websocket.send(json.dumps(inject_response_fields(resp, msg)))

        elif cmd == "heartbeat":
            await websocket.send(json.dumps(inject_response_fields({"status": "ok"}, msg)))

        elif cmd == "pause":
            state["paused"] = True
            await websocket.send(json.dumps(inject_response_fields({"status": "ok", "paused": True}, msg)))

        elif cmd == "resume":
            state["paused"] = False
            await websocket.send(json.dumps(inject_response_fields({"status": "ok", "paused": False}, msg)))

    async def _handle_subscribe(self, websocket, msg, state):
        """ADDITIVE subscribe — merge newly-requested topics; already-subscribed
        topics are a no-op (no schema echoed), matching production."""
        requested = parse_topic_names(msg.get("topics", []))
        known = {t["name"]: t for t in TOPICS}
        schemas = {}
        failures = []
        for name in requested:
            if name in state["subscribed"]:
                continue
            topic = known.get(name)
            if topic is None:
                failures.append({"topic": name, "reason": "Topic does not exist"})
                continue
            state["subscribed"].add(name)
            schemas[name] = {"encoding": topic["encoding"], "definition": topic["definition"]}

        resp: dict = {}
        if not failures:
            resp["status"] = "success"
        elif not schemas:
            resp["status"] = "error"
            resp["error_code"] = "ALL_SUBSCRIPTIONS_FAILED"
            resp["message"] = "Failed to subscribe to all requested topics"
        else:
            resp["status"] = "partial_success"
            resp["message"] = "Some subscriptions failed"
        resp["schemas"] = schemas
        if failures:
            resp["failures"] = failures
        await websocket.send(json.dumps(inject_response_fields(resp, msg)))
        print(f"    subscribe → {resp['status']}; schemas={sorted(schemas)}; failures={[f['topic'] for f in failures]}")

    async def emit_loop(self):
        t = 0.0
        while True:
            await asyncio.sleep(0.1)
            t += 0.1

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
            image_cdr = encode_compressed_image_cdr(t)

            for websocket, state in list(self.clients.items()):
                if state["paused"]:
                    continue
                subscribed = state["subscribed"]
                if not subscribed:
                    continue

                messages = []
                for topic, cdr in scalar_payloads.items():
                    if topic in subscribed:
                        messages.append((topic, ts_ns, cdr))
                if "/camera/image" in subscribed:
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
