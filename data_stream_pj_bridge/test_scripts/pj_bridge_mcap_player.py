#!/usr/bin/env python3
"""
PJ Bridge MCAP player — replays an MCAP file via the PJ Bridge WebSocket protocol.

Reads any MCAP file and republishes its messages continuously using the
PlotJuggler Bridge binary protocol (PJRB + zstd).

Usage:
    pip install websockets zstandard mcap
    python3 pj_bridge_mcap_player.py path/to/file.mcap [--port PORT] [--speed SPEED]

    --speed 1.0  = real time (default)
    --speed 0.0  = as fast as possible
"""

import argparse
import asyncio
import json
import struct
import time
from pathlib import Path

import websockets
import zstandard as zstd
from mcap.reader import make_reader

PORT = 9871
HOST = "localhost"
MAGIC = 0x42524A50  # "PJRB" LE


def load_mcap_metadata(path: str):
    """Load channels and schemas from MCAP summary."""
    with open(path, "rb") as f:
        r = make_reader(f)
        s = r.get_summary()
        channels = {}
        if s:
            schemas = s.schemas or {}
            for ch_id, ch in (s.channels or {}).items():
                sch = schemas.get(ch.schema_id)
                channels[ch_id] = {
                    "name": ch.topic,
                    "type": sch.name if sch else "",
                    "schema_name": sch.name if sch else "",
                    "encoding": sch.encoding if sch else "cdr",
                    "definition": sch.data.decode("utf-8", errors="replace") if sch and sch.data else "",
                }
        return channels


def read_messages(path: str):
    """Generator: yields (channel_id, log_time_ns, data)."""
    with open(path, "rb") as f:
        r = make_reader(f)
        for schema, channel, message in r.iter_messages():
            yield channel.id, message.log_time, message.data


def build_binary_frame(messages: list) -> bytes:
    """PJ Bridge binary frame: PJRB header + zstd payload."""
    payload = bytearray()
    for topic, ts_ns, data in messages:
        tb = topic.encode("utf-8")
        payload += struct.pack("<H", len(tb)) + tb
        payload += struct.pack("<q", ts_ns)
        payload += struct.pack("<I", len(data)) + data
    compressed = zstd.ZstdCompressor().compress(bytes(payload))
    return struct.pack("<IIII", MAGIC, len(messages), len(payload), 0) + compressed


class PjBridgeMcapPlayer:
    def __init__(self, mcap_path: str, speed: float):
        self.mcap_path = mcap_path
        self.speed = speed
        self.channels = load_mcap_metadata(mcap_path)
        self.clients: dict = {}  # websocket → {paused, subscribed_topics}

    def _topics_list(self):
        return [
            {
                "name": ch["name"],
                "type": ch["type"],
                "schema_name": ch["schema_name"],
                "encoding": "cdr",
                "definition": ch["definition"],
            }
            for ch in self.channels.values()
        ]

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

    async def _on_text(self, websocket, raw):
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            return
        cmd = msg.get("command", "")

        if cmd == "get_topics":
            await websocket.send(json.dumps({
                "status": "success",
                "topics": self._topics_list(),
            }))
            print(f"    get_topics → {[ch['name'] for ch in self.channels.values()]}")

        elif cmd == "subscribe":
            requested = set(msg.get("topics", []))
            self.clients[websocket]["subscribed_topics"] = requested

            schemas = {}
            for ch in self.channels.values():
                if ch["name"] in requested:
                    schemas[ch["name"]] = {
                        "encoding": "cdr",
                        "definition": ch["definition"],
                        "schema_name": ch["schema_name"],
                    }
            await websocket.send(json.dumps({"status": "success", "schemas": schemas}))
            print(f"    subscribe → {sorted(requested)}")

        elif cmd == "pause":
            self.clients[websocket]["paused"] = True
        elif cmd == "resume":
            self.clients[websocket]["paused"] = False

    async def play_loop(self):
        # Build topic name lookup
        ch_topic = {ch_id: ch["name"] for ch_id, ch in self.channels.items()}

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

            # Group messages by timestamp bucket (~100ms) for efficient frames
            bucket_ms = 100
            bucket: list = []
            bucket_ts = first_ts

            for ch_id, log_time_ns, data in messages:
                if not self.clients:
                    await asyncio.sleep(0.01)
                    continue

                topic = ch_topic.get(ch_id)
                if topic is None:
                    continue

                # Timing
                if self.speed > 0:
                    bag_elapsed = (log_time_ns - first_ts) / 1e9
                    wall_elapsed = time.monotonic() - wall_start
                    wait = bag_elapsed / self.speed - wall_elapsed
                    if wait > 0:
                        await asyncio.sleep(wait)

                bucket.append((topic, int(time.time() * 1e9), data))

                # Flush bucket every 100ms of bag time
                if (log_time_ns - bucket_ts) >= bucket_ms * 1_000_000 or len(bucket) >= 50:
                    await self._send_bucket(bucket)
                    bucket = []
                    bucket_ts = log_time_ns

            if bucket:
                await self._send_bucket(bucket)

            print(f"    loop {loop_count} done, restarting...")

    async def _send_bucket(self, bucket: list):
        if not bucket:
            return
        for websocket, state in list(self.clients.items()):
            if state["paused"]:
                continue
            subscribed = state["subscribed_topics"]
            msgs = [(t, ts, d) for t, ts, d in bucket if t in subscribed]
            if not msgs:
                continue
            frame = build_binary_frame(msgs)
            try:
                await websocket.send(frame)
                print(f"    → {len(frame)}b  {len(msgs)} msgs", end="\r")
            except websockets.exceptions.ConnectionClosed:
                pass


async def main(mcap_path: str, host: str, port: int, speed: float):
    player = PjBridgeMcapPlayer(mcap_path, speed)
    topics = [ch["name"] for ch in player.channels.values()]
    print(f"PJ Bridge MCAP player — {Path(mcap_path).name}")
    print(f"Listening on ws://{host}:{port}")
    print(f"Speed: {'real-time' if speed == 1.0 else f'{speed}x' if speed > 0 else 'max'}")
    print(f"Topics ({len(topics)}): {topics}")
    print("Ctrl+C to stop\n")

    async with websockets.serve(player.handler, host, port):
        await player.play_loop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="PJ Bridge MCAP player")
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
