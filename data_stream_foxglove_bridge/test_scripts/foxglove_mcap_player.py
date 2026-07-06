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
import collections
import json
import re
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
                    # MCAP writers use "proto"/"protobuf" interchangeably; the ws-protocol
                    # vocabulary is "protobuf" and clients classify by it.
                    "schemaEncoding": ("protobuf" if sch and sch.encoding == "proto" else (sch.encoding if sch else "")),
                    "metadata": dict(ch.metadata) if ch.metadata else {},
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


def is_latched_channel(ch: dict, extra_latched: set) -> bool:
    """transient_local emulation: which channels replay to late subscribers.

    Preferred signal: rosbag2's offered_qos_profiles channel metadata (absent in
    many bags). Fallback: conventional latched topics — *_static (tf_static),
    /map, /robot_description — plus any --latch override.
    """
    topic = ch["topic"]
    if topic in extra_latched or topic.endswith("_static"):
        return True
    if topic in ("/map", "/robot_description"):
        return True
    qos = ch.get("metadata", {}).get("offered_qos_profiles", "").lower()
    # Covers both YAML vocabularies: "durability: 1" (old enum) and
    # "durability: transient_local" (jazzy+). The leading word boundary keeps
    # "durability: 1" from also matching "max_durability: 1".
    return "transient_local" in qos or re.search(r"\bdurability:\s*1\b", qos) is not None


class FoxgloveMcapPlayer:
    # Cap on the latched-message replay cache per channel.
    _LATCH_CAP = 16

    # Wall-clock seconds above which an inter-message gap is treated as dead air
    # and skipped (pacing re-anchors to now) instead of slept through. Real bags
    # log transient_local topics (/tf_static, /map) at open, often tens of
    # seconds before the data burst; without this the player would sleep through
    # that whole gap and a client would see nothing after the latched replay.
    # This compresses ANY idle gap past the threshold, not just the leading one —
    # a deliberate test-player choice (skip dead air); it is not a precise pause
    # boundary, so an intentional multi-second pause in a bag is skipped too.
    _MAX_IDLE_GAP_S = 1.0

    def __init__(self, mcap_path: str, speed: float, loop: bool, extra_latched: set):
        self.mcap_path = mcap_path
        self.speed = speed
        self.loop = loop
        self.channels = load_mcap_channels(mcap_path)
        self.latched_channel_ids = {
            ch_id for ch_id, ch in self.channels.items() if is_latched_channel(ch, extra_latched)
        }
        self.clients: dict = {}  # websocket → {channel_id → subscription_id}
        self._readvertise_tasks: dict = {}
        # transient_local emulation: /tf_static (and *_static generally) is
        # published once at bag start; a real foxglove bridge re-delivers the
        # latched message(s) to LATE subscribers. Without this, a client that
        # connects mid-playback never learns the static frames and every
        # sensor-frame TF lookup fails. channel_id → [(log_time_ns, data), ...]
        self.latched: dict = {}

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
                    # Replay latched messages (tf_static, /map) that streamed
                    # past before this subscription existed. Snapshot the deque
                    # first: play_loop may append to it at the await below, and
                    # we only want messages latched up to now, not ones that
                    # arrive mid-replay.
                    for log_time_ns, data in list(self.latched.get(ch_id, ())):
                        try:
                            await websocket.send(build_binary_frame(sub_id, log_time_ns, data))
                        except websockets.exceptions.ConnectionClosed:
                            break
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
            # Stream straight off the reader. Materializing the file
            # (list(read_messages(...))) holds every DECOMPRESSED payload in RAM
            # at once — tens of GiB for a real robot bag — and delays playback
            # until the whole file has been read. Memory now stays bounded by
            # one chunk, and the first message plays immediately.
            # Don't consume the file until a client is connected — otherwise the
            # single pass streams past (advancing the generator) with nobody
            # listening, and a client that connects a moment later gets only
            # advertisements + latched replay, never the live data.
            while not self.clients:
                await asyncio.sleep(0.05)

            first_ts = None
            wall_start = time.monotonic()

            for ch_id, log_time_ns, data in read_messages(self.mcap_path):
                if first_ts is None:
                    first_ts = log_time_ns
                    wall_start = time.monotonic()

                # Latched channels (tf_static, /map, ...): keep the most recent
                # messages so a late subscriber can be caught up (see _on_text
                # "subscribe"). A bounded deque keeps the LATEST — transient_local
                # semantics are last-value-wins, so a re-latched /map replays its
                # newest state, not its first.
                if ch_id in self.latched_channel_ids:
                    cache = self.latched.get(ch_id)
                    if cache is None:
                        cache = collections.deque(maxlen=self._LATCH_CAP)
                        self.latched[ch_id] = cache
                    cache.append((log_time_ns, data))

                if not self.clients:
                    await asyncio.sleep(0.01)
                    continue

                # Timing: wait until the right wall-clock moment
                if self.speed > 0:
                    bag_elapsed = (log_time_ns - first_ts) / 1e9
                    wall_elapsed = time.monotonic() - wall_start
                    wait = bag_elapsed / self.speed - wall_elapsed
                    if wait > self._MAX_IDLE_GAP_S:
                        # Long idle gap (a transient_local topic logged at open,
                        # a recording pause): re-anchor pacing to now so the next
                        # message streams immediately instead of stalling.
                        first_ts = log_time_ns
                        wall_start = time.monotonic()
                    elif wait > 0:
                        await asyncio.sleep(wait)

                # Send to all subscribed clients, with the ORIGINAL log time.
                # The embedded header stamps inside the payloads are bag time and
                # cannot be rewritten, so the frame timestamp must match them or
                # every consumer sees two inconsistent time axes (TF stamped at
                # bag time vs samples stored at receive time). This matches a
                # real bridge replaying under use_sim_time.
                for websocket, subscriptions in list(self.clients.items()):
                    sub_id = subscriptions.get(ch_id)
                    if sub_id is None:
                        continue
                    frame = build_binary_frame(sub_id, log_time_ns, data)
                    try:
                        await websocket.send(frame)
                    except websockets.exceptions.ConnectionClosed:
                        pass

            if first_ts is None:  # empty file: nothing was yielded
                await asyncio.sleep(1.0)
                continue
            if not self.loop:
                # Timestamps rewind on a replay, which live consumers reject
                # (non-monotonic ingest) — so looping is opt-in. Keep the server
                # alive so connected clients stay up (advertise keeps working).
                print(f"    playback finished — server stays up (pass --loop to repeat; time rewinds each loop)")
                while True:
                    await asyncio.sleep(3600)
            print(f"    loop {loop_count} done, restarting...")


async def main(mcap_path: str, host: str, port: int, speed: float, loop: bool, extra_latched: set):
    player = FoxgloveMcapPlayer(mcap_path, speed, loop, extra_latched)
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
    parser.add_argument("--latch", default="",
                        help="Comma-separated topics to latch (replayed to late subscribers) "
                             "in addition to QoS-transient_local, *_static, /map, /robot_description.")
    parser.add_argument("--loop", action="store_true",
                        help="Restart playback when the file ends. Timestamps rewind "
                             "on each pass, which live consumers may reject.")
    args = parser.parse_args()

    try:
        extra_latched = {t.strip() for t in args.latch.split(",") if t.strip()}
        asyncio.run(main(args.mcap, args.host, args.port, args.speed, args.loop, extra_latched))
    except KeyboardInterrupt:
        print("\nStopped.")
