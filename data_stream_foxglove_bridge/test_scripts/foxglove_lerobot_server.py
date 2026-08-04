#!/usr/bin/env python3
"""
Foxglove WebSocket server replicating LeRobot 0.6.0's --display_mode=foxglove.

Publishes exactly what lerobot's foxglove_visualization.py publishes:
  - /observation/state        json/jsonschema, {"scalars":[{"label","value"}...]} @ 30 Hz
  - /action/state             same shape, phase-shifted
  - /observation/images/front protobuf foxglove.RawImage (well-known, schemaless), rgb8 @ 5 Hz

The scalars schema below is a verbatim copy of lerobot's _SCALARS_SCHEMA; the
channel/topic names and payload shape match src/lerobot/utils/foxglove_visualization.py.

Usage:
    pip install websockets
    python3 foxglove_lerobot_server.py [--port PORT] [--host HOST]
"""

import argparse
import asyncio
import json
import math
import struct
import time

import websockets

PORT = 8765
HOST = "localhost"

MESSAGE_DATA_OPCODE = 0x01

# Verbatim from lerobot src/lerobot/utils/foxglove_visualization.py (_SCALARS_SCHEMA).
SCALARS_SCHEMA = {
    "type": "object",
    "title": "lerobot.Scalars",
    "properties": {
        "scalars": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "label": {"type": "string"},
                    "value": {"type": "number"},
                },
            },
        }
    },
}

JOINTS = ["shoulder_pan.pos", "shoulder_lift.pos", "elbow_flex.pos", "wrist_flex.pos", "wrist_roll.pos", "gripper.pos"]

CHANNELS = [
    {
        "id": 1,
        "topic": "/observation/state",
        "encoding": "json",
        "schemaName": "lerobot.Scalars",
        "schema": json.dumps(SCALARS_SCHEMA),
        "schemaEncoding": "jsonschema",
    },
    {
        "id": 2,
        "topic": "/action/state",
        "encoding": "json",
        "schemaName": "lerobot.Scalars",
        "schema": json.dumps(SCALARS_SCHEMA),
        "schemaEncoding": "jsonschema",
    },
    {
        # Camera leg: well-known foxglove.RawImage, schemaless (parser_protobuf
        # decodes it by name) — proves the typed protobuf route still works in
        # the same session as the new json route.
        "id": 3,
        "topic": "/observation/images/front",
        "encoding": "protobuf",
        "schemaName": "foxglove.RawImage",
        "schema": "",
        "schemaEncoding": "protobuf",
    },
]

CHANNEL_BY_ID = {ch["id"]: ch for ch in CHANNELS}

WIDTH, HEIGHT = 160, 120


def scalars_payload(t: float, phase: float) -> bytes:
    scalars = [
        {"label": name, "value": math.sin(t * (0.4 + 0.13 * i) + phase + i)}
        for i, name in enumerate(JOINTS)
    ]
    return json.dumps({"scalars": scalars}).encode()


def _pb_len_delimited(field: int, payload: bytes) -> bytes:
    return _pb_varint((field << 3) | 2) + _pb_varint(len(payload)) + payload


def _pb_varint(v: int) -> bytes:
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def raw_image_payload(t: float) -> bytes:
    """Manually protobuf-encode a foxglove.RawImage (official field numbers)."""
    row = bytearray()
    shift = int(t * 40) % 256
    for x in range(WIDTH):
        row += bytes(((x + shift) % 256, (2 * x) % 256, 255 - (x + shift) % 256))
    data = bytes(row) * HEIGHT

    ns = time.time_ns()
    timestamp = _pb_varint(1 << 3) + _pb_varint(ns // 10**9) + _pb_varint(2 << 3) + _pb_varint(ns % 10**9)

    msg = bytearray()
    msg += _pb_len_delimited(1, timestamp)                    # timestamp
    msg += _pb_len_delimited(2, b"front")                     # frame_id
    msg += struct.pack("<BI", (3 << 3) | 5, WIDTH)            # width  (fixed32)
    msg += struct.pack("<BI", (4 << 3) | 5, HEIGHT)           # height (fixed32)
    msg += _pb_len_delimited(5, b"rgb8")                      # encoding
    msg += struct.pack("<BI", (6 << 3) | 5, WIDTH * 3)        # step   (fixed32)
    msg += _pb_len_delimited(7, data)                         # data
    return bytes(msg)


def build_binary_frame(subscription_id: int, log_time_ns: int, payload: bytes) -> bytes:
    """[opcode:1][subscription_id:4 LE][log_time_ns:8 LE][payload]"""
    return struct.pack("<BIQ", MESSAGE_DATA_OPCODE, subscription_id, log_time_ns) + payload


class FoxgloveLerobotServer:
    def __init__(self):
        self.clients: dict = {}

    async def handler(self, websocket):
        client_id = id(websocket)
        self.clients[websocket] = {}
        print(f"[+] Client connected: {client_id}")

        await websocket.send(json.dumps({"op": "advertise", "channels": CHANNELS}))
        print(f"    advertise → {[ch['topic'] for ch in CHANNELS]}")

        task = asyncio.create_task(self._readvertise_until_subscribed(websocket))

        try:
            async for message in websocket:
                await self.on_message(websocket, message)
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            task.cancel()
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
        """Scalars at 30 Hz, one RawImage every 6th tick (5 Hz)."""
        interval = 1.0 / 30.0
        t = 0.0
        tick = 0
        while True:
            await asyncio.sleep(interval)
            t += interval
            tick += 1

            if not self.clients:
                continue

            log_time_ns = time.time_ns()
            payloads = {
                1: scalars_payload(t, 0.0),
                2: scalars_payload(t, math.pi / 3),
            }
            if tick % 6 == 0:
                payloads[3] = raw_image_payload(t)

            for websocket, subscriptions in list(self.clients.items()):
                for channel_id, sub_id in list(subscriptions.items()):
                    payload = payloads.get(channel_id)
                    if payload is None:
                        continue
                    try:
                        await websocket.send(build_binary_frame(sub_id, log_time_ns, payload))
                    except websockets.exceptions.ConnectionClosed:
                        pass

            print(f"    → t={t:.1f}s", end="\r")


async def main(host: str, port: int):
    server = FoxgloveLerobotServer()
    print(f"LeRobot-style Foxglove server listening on ws://{host}:{port}")
    print("Topics: /observation/state, /action/state (json 30 Hz), /observation/images/front (RawImage 5 Hz)")
    print("Ctrl+C to stop\n")

    async with websockets.serve(
        server.handler,
        host,
        port,
        subprotocols=["foxglove.sdk.v1"],
        max_size=None,
    ):
        await server.emit_loop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="LeRobot 0.6.0-style Foxglove server (json scalars + RawImage)")
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()

    try:
        asyncio.run(main(args.host, args.port))
    except KeyboardInterrupt:
        print("\nStopped.")
