#!/usr/bin/env python3
"""
PJ Bridge MCAP player — replays an MCAP file via the PJ Bridge WebSocket protocol.

Reads any MCAP file and republishes its messages continuously using the
PlotJuggler Bridge binary protocol (PJRB + zstd).

The JSON control plane mirrors the PRODUCTION server semantics
(github.com/PlotJuggler/plotjuggler_bridge): additive subscribe, unsubscribe
with a `removed` list, include_schemas on get_topics, subscribe_topic_updates
opt-in, and `id` + `protocol_version` echoed on every response. An MCAP file's
channel set is fully known at open, so this player implements the topic_updates
opt-in but never pushes a topics_changed notification — there is nothing that
materializes over time to announce.

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
PROTOCOL_VERSION = 1


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


def topic_entry(channel: dict, include_schemas: bool) -> dict:
    """{name, type} by default; adds {encoding, definition} when requested."""
    entry = {"name": channel["name"], "type": channel["type"]}
    if include_schemas:
        entry["encoding"] = channel["encoding"]
        entry["definition"] = channel["definition"]
    return entry


def load_mcap_metadata(path: str):
    """Load channels and schemas from the MCAP summary. The wire `encoding` is the
    channel's message_encoding (e.g. "cdr" for ROS2, "json" for JSON channels) —
    the serialization the plugin's parser needs — not the schema language."""
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
                    "encoding": ch.message_encoding or "cdr",
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
        self.clients: dict = {}  # websocket → {paused, subscribed, topic_updates, tu_include_schemas}

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

    async def _on_text(self, websocket, raw):
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            return
        if not isinstance(msg, dict):
            return

        cmd = msg.get("command", "")
        state = self.clients[websocket]

        if cmd == "get_topics":
            include = bool(msg.get("include_schemas", False))
            resp = {"status": "success",
                    "topics": [topic_entry(ch, include) for ch in self.channels.values()]}
            await websocket.send(json.dumps(inject_response_fields(resp, msg)))
            print(f"    get_topics (include_schemas={include}) → {len(self.channels)} topics")

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
        known = {ch["name"]: ch for ch in self.channels.values()}
        schemas = {}
        failures = []
        for name in requested:
            if name in state["subscribed"]:
                continue
            ch = known.get(name)
            if ch is None:
                failures.append({"topic": name, "reason": "Topic does not exist"})
                continue
            state["subscribed"].add(name)
            schemas[name] = {"encoding": ch["encoding"], "definition": ch["definition"]}

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
            subscribed = state["subscribed"]
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
