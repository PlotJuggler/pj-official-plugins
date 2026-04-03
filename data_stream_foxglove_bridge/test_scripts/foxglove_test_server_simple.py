#!/usr/bin/env python3
"""
Foxglove WebSocket test server (simple) — no external dependencies beyond websockets.

Implements the Foxglove WebSocket Protocol (sdk.v1) manually so that the
data_stream_foxglove_bridge plugin can connect and receive data.
Use this if you cannot install the foxglove-websocket package.

Usage:
    pip install websockets
    python3 foxglove_test_server_simple.py [--port PORT] [--host HOST]

For the full version using the official Foxglove SDK:
    pip install foxglove-websocket
    python3 foxglove_test_server.py

Protocol summary:
    Server → Client  JSON: {"op": "advertise", "channels": [...]}
    Client → Server  JSON: {"op": "subscribe", "subscriptions": [...]}
    Server → Client  Binary frames: [opcode:1][sub_id:4 LE][log_time_ns:8 LE][cdr]
    Client → Server  JSON: {"op": "unsubscribe", "subscriptionIds": [...]}

Channel requirements (enforced by plugin):
    encoding        = "cdr"
    schemaEncoding  = "ros2msg"
    schema          = ros2 .msg definition text

CDR encoding: 4-byte encapsulation header [0x00 0x01 0x00 0x00] + payload bytes.
"""

import argparse
import asyncio
import json
import math
import struct
import time

import websockets
from websockets.legacy.server import WebSocketServerProtocol

PORT = 8765
HOST = "localhost"

MESSAGE_DATA_OPCODE = 0x01

CHANNELS = [
    {
        "id": 1,
        "topic": "/test/sine",
        "encoding": "cdr",
        "schemaName": "std_msgs/msg/Float64",
        "schema": "float64 data\n",
        "schemaEncoding": "ros2msg",
    },
    {
        "id": 2,
        "topic": "/test/cosine",
        "encoding": "cdr",
        "schemaName": "std_msgs/msg/Float64",
        "schema": "float64 data\n",
        "schemaEncoding": "ros2msg",
    },
    {
        "id": 3,
        "topic": "/test/sawtooth",
        "encoding": "cdr",
        "schemaName": "std_msgs/msg/Float64",
        "schema": "float64 data\n",
        "schemaEncoding": "ros2msg",
    },
]

CHANNEL_BY_ID = {ch["id"]: ch for ch in CHANNELS}


def encode_float64_cdr(value: float) -> bytes:
    """CDR-encode a float64 for std_msgs/msg/Float64.
    nanocdr::Decoder expects the 4-byte CDR encapsulation header:
      [0x00][0x01=LE][0x00][0x00] followed by the payload bytes."""
    return b"\x00\x01\x00\x00" + struct.pack("<d", value)


def build_binary_frame(subscription_id: int, log_time_ns: int, cdr: bytes) -> bytes:
    """[opcode:1][subscription_id:4 LE][log_time_ns:8 LE][cdr_payload]"""
    return struct.pack("<BIQ", MESSAGE_DATA_OPCODE, subscription_id, log_time_ns) + cdr


class FoxgloveTestServer:
    def __init__(self):
        # ws -> {channel_id -> subscription_id}
        self.clients: dict[WebSocketServerProtocol, dict[int, int]] = {}
        # ws -> asyncio.Task (per-client re-advertise until subscribed)
        self._readvertise_tasks: dict[WebSocketServerProtocol, asyncio.Task] = {}

    async def handler(self, websocket: WebSocketServerProtocol):
        client_id = id(websocket)
        self.clients[websocket] = {}
        print(f"[+] Client connected: {client_id}")

        await websocket.send(json.dumps({"op": "advertise", "channels": CHANNELS}))
        print(f"    advertise → {[ch['topic'] for ch in CHANNELS]}")

        # Re-advertise every 2s until the plugin subscribes (handles dialog→source handoff)
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

    async def _readvertise_until_subscribed(self, websocket: WebSocketServerProtocol):
        """Send advertise every 2s until the client has subscribed.
        Handles the dialog→source socket handoff in the plugin."""
        try:
            while True:
                await asyncio.sleep(2.0)
                if not self.clients.get(websocket):
                    await websocket.send(json.dumps({"op": "advertise", "channels": CHANNELS}))
                    print("    re-advertise → waiting for subscribe")
                else:
                    print("    re-advertise → already subscribed, stopping")
                    break
        except (asyncio.CancelledError, websockets.exceptions.ConnectionClosed):
            pass

    async def on_message(self, websocket: WebSocketServerProtocol, message):
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
        """Send data frames to all subscribed clients at ~10 Hz."""
        t = 0.0
        while True:
            await asyncio.sleep(0.1)
            t += 0.1

            if not self.clients:
                continue

            log_time_ns = int(time.time() * 1e9)
            values = {
                1: math.sin(t),
                2: math.cos(t),
                3: (t % (2 * math.pi)) / (2 * math.pi),
            }

            for websocket, subscriptions in list(self.clients.items()):
                if not subscriptions:
                    continue
                for channel_id, sub_id in list(subscriptions.items()):
                    if channel_id not in values:
                        continue
                    frame = build_binary_frame(sub_id, log_time_ns, encode_float64_cdr(values[channel_id]))
                    try:
                        await websocket.send(frame)
                    except websockets.exceptions.ConnectionClosed:
                        pass

            topic_names = [CHANNEL_BY_ID[ch_id]["topic"] for ch_id in values]
            print(f"    → frame → {topic_names}", end="\r")


async def main(host: str, port: int):
    server = FoxgloveTestServer()
    print(f"Foxglove test server (simple) listening on ws://{host}:{port}")
    print(f"Subprotocol: foxglove.sdk.v1")
    print(f"Channels: {[ch['topic'] for ch in CHANNELS]}")
    print("Ctrl+C to stop\n")

    async with websockets.serve(
        server.handler,
        host,
        port,
        subprotocols=["foxglove.sdk.v1"],
    ):
        await server.emit_loop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Foxglove WebSocket test server (simple, no foxglove-websocket SDK)")
    parser.add_argument("--host", default=HOST, help=f"Bind address (default: {HOST})")
    parser.add_argument("--port", type=int, default=PORT, help=f"Port (default: {PORT})")
    args = parser.parse_args()

    try:
        asyncio.run(main(args.host, args.port))
    except KeyboardInterrupt:
        print("\nStopped.")
