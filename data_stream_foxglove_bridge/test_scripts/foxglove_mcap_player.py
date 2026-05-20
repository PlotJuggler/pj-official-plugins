#!/usr/bin/env python3
"""
Foxglove WebSocket MCAP player — replays an MCAP file as a live stream.

Reads any MCAP file and republishes its messages via the Foxglove
WebSocket protocol (sdk.v1), looping the playback continuously.
Useful for testing the data_stream_foxglove_bridge plugin with real data.

Usage:
    pip install websockets mcap
    python3 foxglove_mcap_player.py path/to/file.mcap [--port PORT] [--speed SPEED]

    --speed 1.0  = real time (default)
    --speed 2.0  = double speed
    --speed 0.0  = as fast as possible
"""

import argparse
import asyncio
import json
import struct
import time
from pathlib import Path

import websockets
from mcap.reader import make_reader

PORT = 8765
HOST = "localhost"
MESSAGE_DATA_OPCODE = 0x01


def load_mcap_channels(path: str):
    """Read all channels and their schemas from an MCAP file."""
    with open(path, "rb") as f:
        r = make_reader(f)
        s = r.get_summary()
        channels = {}
        schemas = {}
        if s:
            for sch_id, sch in (s.schemas or {}).items():
                schemas[sch_id] = sch
            for ch_id, ch in (s.channels or {}).items():
                sch = schemas.get(ch.schema_id)
                channels[ch_id] = {
                    "id": ch_id,
                    "topic": ch.topic,
                    "encoding": ch.message_encoding,
                    "schemaName": sch.name if sch else "",
                    "schema": sch.data.decode("utf-8", errors="replace") if sch and sch.data else "",
                    "schemaEncoding": sch.encoding if sch else "",
                }
        return channels


def read_messages(path: str):
    """Generator: yields (channel_id, log_time_ns, data) for every message."""
    with open(path, "rb") as f:
        r = make_reader(f)
        for schema, channel, message in r.iter_messages():
            yield channel.id, message.log_time, message.data


def build_binary_frame(subscription_id: int, log_time_ns: int, data: bytes) -> bytes:
    return struct.pack("<BIQ", MESSAGE_DATA_OPCODE, subscription_id, log_time_ns) + data


class FoxgloveMcapPlayer:
    def __init__(self, mcap_path: str, speed: float):
        self.mcap_path = mcap_path
        self.speed = speed
        self.channels = load_mcap_channels(mcap_path)
        self.clients: dict = {}  # websocket → {channel_id → subscription_id}
        self._readvertise_tasks: dict = {}

        # Build advertise list from channels
        self.advertise_channels = [
            {
                "id": ch["id"],
                "topic": ch["topic"],
                "encoding": ch["encoding"],
                "schemaName": ch["schemaName"],
                "schema": ch["schema"],
                "schemaEncoding": ch["schemaEncoding"],
            }
            for ch in self.channels.values()
        ]

    async def handler(self, websocket):
        self.clients[websocket] = {}
        print(f"[+] Client connected")
        await websocket.send(json.dumps({"op": "advertise", "channels": self.advertise_channels}))

        task = asyncio.create_task(self._readvertise(websocket))
        self._readvertise_tasks[websocket] = task

        try:
            async for message in websocket:
                if isinstance(message, bytes):
                    continue
                await self._on_text(websocket, message)
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            task.cancel()
            self._readvertise_tasks.pop(websocket, None)
            self.clients.pop(websocket, None)
            print(f"[-] Client disconnected")

    async def _readvertise(self, websocket):
        try:
            while True:
                await asyncio.sleep(2.0)
                if not self.clients.get(websocket):
                    await websocket.send(json.dumps({"op": "advertise", "channels": self.advertise_channels}))
        except (asyncio.CancelledError, websockets.exceptions.ConnectionClosed):
            pass

    async def _on_text(self, websocket, raw):
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            return
        op = msg.get("op", "")
        if op == "subscribe":
            for sub in msg.get("subscriptions", []):
                sub_id = sub.get("id")
                ch_id = sub.get("channelId")
                if sub_id is not None and ch_id in self.channels:
                    self.clients[websocket][ch_id] = sub_id
            subscribed = [self.channels[c]["topic"] for c in self.clients[websocket]]
            print(f"    subscribe → {subscribed}")
        elif op == "unsubscribe":
            for sub_id in msg.get("subscriptionIds", []):
                self.clients[websocket] = {
                    ch: s for ch, s in self.clients[websocket].items() if s != sub_id
                }

    async def play_loop(self):
        loop_count = 0
        while True:
            loop_count += 1
            print(f"\n→ Loop {loop_count} — reading {Path(self.mcap_path).name}")
            messages = list(read_messages(self.mcap_path))
            if not messages:
                await asyncio.sleep(1.0)
                continue

            first_ts = messages[0][1]
            wall_start = time.monotonic()

            for ch_id, log_time_ns, data in messages:
                if not self.clients:
                    await asyncio.sleep(0.01)
                    continue

                # Timing: wait until the right wall-clock moment
                if self.speed > 0:
                    bag_elapsed = (log_time_ns - first_ts) / 1e9
                    wall_elapsed = time.monotonic() - wall_start
                    wait = bag_elapsed / self.speed - wall_elapsed
                    if wait > 0:
                        await asyncio.sleep(wait)

                # Send to all subscribed clients
                now_ns = int(time.time() * 1e9)
                for websocket, subscriptions in list(self.clients.items()):
                    sub_id = subscriptions.get(ch_id)
                    if sub_id is None:
                        continue
                    frame = build_binary_frame(sub_id, now_ns, data)
                    try:
                        await websocket.send(frame)
                    except websockets.exceptions.ConnectionClosed:
                        pass

            print(f"    loop {loop_count} done, restarting...")


async def main(mcap_path: str, host: str, port: int, speed: float):
    player = FoxgloveMcapPlayer(mcap_path, speed)
    topics = [ch["topic"] for ch in player.channels.values()]
    print(f"Foxglove MCAP player — {Path(mcap_path).name}")
    print(f"Listening on ws://{host}:{port}")
    print(f"Speed: {'real-time' if speed == 1.0 else f'{speed}x' if speed > 0 else 'max'}")
    print(f"Topics ({len(topics)}): {topics}")
    print("Ctrl+C to stop\n")

    async with websockets.serve(
        player.handler, host, port,
        subprotocols=["foxglove.sdk.v1"],
    ):
        await player.play_loop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Foxglove WebSocket MCAP player")
    parser.add_argument("mcap", help="Path to MCAP file")
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--speed", type=float, default=1.0,
                        help="Playback speed (1.0=real-time, 0=max)")
    args = parser.parse_args()

    try:
        asyncio.run(main(args.mcap, args.host, args.port, args.speed))
    except KeyboardInterrupt:
        print("\nStopped.")
