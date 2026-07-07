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
import queue
import struct
import sys
import threading
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
    """{name, type} by default; adds {encoding, definition} when requested;
    `latched: true` for transient-local channels (production badge — absent
    means not latched or unknown)."""
    entry = {"name": channel["name"], "type": channel["type"]}
    if channel.get("latched"):
        entry["latched"] = True
    if include_schemas:
        entry["encoding"] = channel["encoding"]
        entry["definition"] = channel["definition"]
    return entry


def wire_encoding(message_encoding: str, schema_encoding: str) -> str:
    """The control-plane `encoding` field is what PlotJuggler resolves a PARSER
    with (production sends schema_encoding(): "ros2msg"/"omgidl") — NOT the
    payload serialization. "cdr" resolves no parser at all (parser_ros registers
    ros2msg/omgidl/ros1msg; parser_json registers json/cbor/msgpack/bson): a CDR
    channel must advertise its SCHEMA language, a JSON channel "json"."""
    if message_encoding == "json" or schema_encoding == "jsonschema":
        return "json"
    if schema_encoding:
        return schema_encoding
    return message_encoding


# Shared latched-topic detection (recorded QoS metadata first, name heuristic
# fallback) — one implementation for both bridge test players.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common" / "test_support"))
import mcap_qos  # noqa: E402


def load_latched_messages(path: str, topic_names: set) -> dict:
    """Pre-seed the latch cache from the bag via the mcap index (fast even on a
    multi-GB file): topic -> list of (topic, log_time_ns, data). /tf_static
    typically has exactly ONE message at bag start — without replaying it on
    subscribe, any client arriving later can never resolve the static frames."""
    latched: dict = {}
    if not topic_names:
        return latched
    with open(path, "rb") as f:
        r = make_reader(f)
        for _schema, channel, message in r.iter_messages(topics=list(topic_names)):
            latched.setdefault(channel.topic, []).append((channel.topic, message.log_time, message.data))
            # last-value-wins per transient_local; keep a small tail in case a
            # topic republishes (e.g. several static broadcasters).
            latched[channel.topic] = latched[channel.topic][-16:]
    return latched


def load_mcap_metadata(path: str, extra_latched: frozenset = frozenset()):
    """Load channels and schemas from the MCAP summary."""
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
                    "encoding": wire_encoding(ch.message_encoding or "", sch.encoding if sch else ""),
                    "definition": sch.data.decode("utf-8", errors="replace") if sch and sch.data else "",
                    "latched": mcap_qos.is_latched_channel(ch.topic, dict(ch.metadata or {}), extra_latched),
                }
        return channels


def read_messages(path: str):
    """Generator: yields (channel_id, log_time_ns, data)."""
    with open(path, "rb") as f:
        r = make_reader(f)
        for schema, channel, message in r.iter_messages():
            yield channel.id, message.log_time, message.data


def read_message_chunks(path: str, max_msgs: int = 200, max_bytes: int = 32 * 1024 * 1024):
    """Generator: yields read_messages() output in lists bounded by message
    count AND payload bytes. The play loop streams these through a small queue
    so a multi-gigabyte mcap never materializes in RAM (a 17 GB bag as one
    list() OOM-killed an entire session)."""
    chunk: list = []
    chunk_bytes = 0
    for item in read_messages(path):
        chunk.append(item)
        chunk_bytes += len(item[2])
        if len(chunk) >= max_msgs or chunk_bytes >= max_bytes:
            yield chunk
            chunk = []
            chunk_bytes = 0
    if chunk:
        yield chunk


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
    def __init__(self, mcap_path: str, speed: float, loop: bool = False, extra_latched: frozenset = frozenset()):
        self.mcap_path = mcap_path
        self.speed = speed
        self.loop = loop
        self.channels = load_mcap_metadata(mcap_path, extra_latched)
        latch_topics = {ch["name"] for ch in self.channels.values() if ch["latched"]}
        self.latched = load_latched_messages(mcap_path, latch_topics)
        if self.latched:
            counts = {t: len(m) for t, m in self.latched.items()}
            print(f"Latched topics (replayed on subscribe): {counts}")
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
                    "server": {"name": "pj_bridge_mcap_player", "version": "test",
                               "capabilities": ["include_schemas", "topics_changed", "latched_badge", "latched_replay"]},
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

        # Latched replay, strictly AFTER the subscribe response (the client
        # needs the schema first) — same ordering as the production server.
        replay = [m for name in schemas for m in self.latched.get(name, [])]
        if replay:
            try:
                await websocket.send(build_binary_frame(replay))
                print(f"    latched replay → {[m[0] for m in replay]}")
            except websockets.exceptions.ConnectionClosed:
                pass

    async def play_loop(self):
        # Build topic name lookup
        ch_topic = {ch_id: ch["name"] for ch_id, ch in self.channels.items()}

        loop_count = 0
        while True:
            loop_count += 1
            print(f"\n→ Loop {loop_count} — reading {Path(self.mcap_path).name}")
            if loop_count > 1:
                # Opt-in only: replaying rewinds the ORIGINAL bag timestamps to
                # the start, which live consumers see as non-monotonic time —
                # fine for protocol tests, wrong for TF/3D rendering.
                print("   (looping rewinds bag time — non-monotonic for live consumers)")

            # STREAM the bag through a bounded producer thread — never
            # materialize it (list(read_messages(...)) on a 17 GB bag OOM-killed
            # the whole session). The queue bounds memory to
            # ~maxsize × max_bytes (~128 MB) and the thread keeps file I/O +
            # decompression off the event loop so control-plane requests
            # (subscribe/unsubscribe/heartbeat) stay responsive.
            chunk_queue: queue.Queue = queue.Queue(maxsize=4)

            def produce(q=chunk_queue):
                try:
                    for chunk in read_message_chunks(self.mcap_path):
                        q.put(chunk)
                finally:
                    q.put(None)  # end-of-bag sentinel (also on read error)

            threading.Thread(target=produce, daemon=True).start()

            first_ts = None
            wall_start = time.monotonic()
            messages_seen = 0

            # Group messages by timestamp bucket (~100ms) for efficient frames
            bucket_ms = 100
            bucket: list = []
            bucket_ts = 0

            while True:
                chunk = await asyncio.to_thread(chunk_queue.get)
                if chunk is None:
                    break
                for ch_id, log_time_ns, data in chunk:
                    messages_seen += 1
                    if first_ts is None:
                        first_ts = log_time_ns
                        bucket_ts = first_ts

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

                    # ORIGINAL bag timestamp, never wall clock: the CDR payloads
                    # (TF stamps, headers) carry bag time, so a wall-clock wire
                    # stamp puts every sample months away from its own TF and
                    # scene lookups fail (same fix as the foxglove player).
                    bucket.append((topic, log_time_ns, data))

                    # Flush bucket every 100ms of bag time
                    if (log_time_ns - bucket_ts) >= bucket_ms * 1_000_000 or len(bucket) >= 50:
                        await self._send_bucket(bucket)
                        bucket = []
                        bucket_ts = log_time_ns

            if bucket:
                await self._send_bucket(bucket)

            if messages_seen == 0:
                await asyncio.sleep(1.0)
                continue

            if not self.loop:
                # Single pass by default (foxglove-player parity): keep SERVING
                # (control plane + latched replay stay live) but stop the data
                # stream — looping would rewind bag time (see above). The
                # notice below is deliberately loud: a quiet wire here has
                # repeatedly been mistaken for a client bug.
                print(f"\n■ Bag finished ({messages_seen} msgs) — data stream ended.")
                print("  Restart the player to replay, or pass --loop for wrap-around replay.")
                while True:
                    await asyncio.sleep(3600)

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


async def main(mcap_path: str, host: str, port: int, speed: float, loop: bool = False,
               extra_latched: frozenset = frozenset()):
    player = PjBridgeMcapPlayer(mcap_path, speed, loop, extra_latched)
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
    parser.add_argument("--loop", action="store_true",
                        help="replay the bag forever (rewinds bag time each pass; default: single pass)")
    parser.add_argument("--latch", action="append", default=[],
                        help="extra topic to treat as transient-local (repeatable)")
    parser.add_argument("--speed", type=float, default=1.0,
                        help="Playback speed (1.0=real-time, 0=max)")
    args = parser.parse_args()

    try:
        asyncio.run(main(args.mcap, args.host, args.port, args.speed, args.loop, frozenset(args.latch)))
    except KeyboardInterrupt:
        print("\nStopped.")
