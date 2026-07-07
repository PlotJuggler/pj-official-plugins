#!/usr/bin/env python3
"""
PJ Bridge test server — emits synthetic time series to PlotJuggler.

Implements the server side of the PJ Bridge WebSocket protocol so that the
data_stream_pj_bridge plugin can connect and receive data. The JSON control
plane mirrors the PRODUCTION server semantics
(github.com/PlotJuggler/plotjuggler_bridge) so demand-driven per-topic
subscriptions behave identically here and against the real bridge.

Usage:
    pip install websockets zstandard
    python3 pj_bridge_test_server.py [--port PORT] [--host HOST]
                                     [--late-topic NAME:SECONDS]

Protocol summary (JSON control plane, see production docs/API.md):
    get_topics                → {status:success, topics:[{name,type[,encoding,definition]}]}
    subscribe (ADDITIVE)      → {status, schemas:{topic:{encoding,definition}}, failures:[...]}
    unsubscribe               → {status:success, removed:[names]}
    subscribe_topic_updates   → {status:ok, topic_updates:true}   (+ topics_changed pushes)
    unsubscribe_topic_updates → {status:ok, topic_updates:false}
    heartbeat                 → {status:ok}
    pause / resume            → {status:ok, paused:bool}
Every response echoes the request's `id` (when a string) and carries
`protocol_version: 1`. Binary data frames are unchanged (see build_binary_frame).
"""

import argparse
import asyncio
import json
import math
import struct
import time

import websockets
import zstandard as zstd

PORT = 9871
HOST = "localhost"
PROTOCOL_VERSION = 1

# Topics advertised to the connecting plugin.
# encoding="json" means CDR payload is raw JSON bytes — decoded by parser_json.
BASE_TOPICS = [
    {"name": "/test/sine",     "type": "Float64", "encoding": "json", "definition": ""},
    {"name": "/test/cosine",   "type": "Float64", "encoding": "json", "definition": ""},
    {"name": "/test/sawtooth", "type": "Float64", "encoding": "json", "definition": ""},
]

# Magic: "PJRB" in little-endian = 0x42524A50
MAGIC = 0x42524A50


# ---------------------------------------------------------------------------
# Protocol helpers (identical across all three test servers)
# ---------------------------------------------------------------------------

def inject_response_fields(response: dict, request: dict) -> dict:
    """Mirror production BridgeServer::inject_response_fields: always attach
    protocol_version, and echo the request's `id` when it is a string."""
    response["protocol_version"] = PROTOCOL_VERSION
    rid = request.get("id")
    if isinstance(rid, str):
        response["id"] = rid
    return response


def parse_topic_names(topics) -> list[str]:
    """Extract topic names from a subscribe/unsubscribe `topics` array, which may
    mix plain strings and {"name": ..., "max_rate_hz": ...} objects (production
    accepts both; we ignore rate limits)."""
    names = []
    if isinstance(topics, list):
        for item in topics:
            if isinstance(item, str):
                names.append(item)
            elif isinstance(item, dict) and isinstance(item.get("name"), str):
                names.append(item["name"])
    return names


def topic_entry(topic: dict, include_schemas: bool) -> dict:
    """One get_topics / topics_changed entry: {name, type} by default;
    {name, type, encoding, definition} when include_schemas was requested."""
    entry = {"name": topic["name"], "type": topic["type"]}
    if topic.get("latched"):
        entry["latched"] = True  # explicit per-topic flag in the table (production badge)
    if include_schemas:
        entry["encoding"] = topic["encoding"]
        entry["definition"] = topic["definition"]
    return entry


def build_binary_frame(messages: list[tuple[str, int, bytes]]) -> bytes:
    """
    Build a PJ Bridge binary data frame.

    Frame layout:
        [magic:4 LE][msg_count:4 LE][uncompressed_size:4 LE][flags:4 LE=0]
        [zstd_compressed_payload]

    Each message in the uncompressed payload:
        [topic_len:2 LE][topic:N][timestamp_ns:8 LE][cdr_len:4 LE][cdr:M]

    Args:
        messages: list of (topic_name, timestamp_ns, cdr_bytes)
    """
    payload = bytearray()
    for topic, ts_ns, cdr in messages:
        topic_bytes = topic.encode("utf-8")
        payload += struct.pack("<H", len(topic_bytes))
        payload += topic_bytes
        payload += struct.pack("<q", ts_ns)
        payload += struct.pack("<I", len(cdr))
        payload += cdr

    compressed = zstd.ZstdCompressor().compress(bytes(payload))

    header = struct.pack(
        "<IIII",
        MAGIC,
        len(messages),
        len(payload),  # uncompressed_size (informational)
        0,             # flags = 0
    )
    return header + compressed


def value_for(name: str, t: float) -> float:
    """Synthetic value for a subscribed topic. Known base topics get their named
    waveform; any other (e.g. a --late-topic) gets a default wave so it still
    streams once subscribed."""
    return {
        "/test/sine": math.sin(t),
        "/test/cosine": math.cos(t),
        "/test/sawtooth": (t % (2 * math.pi)) / (2 * math.pi),
    }.get(name, math.sin(t * 2.0))


class PjBridgeServer:
    def __init__(self, late_topic: dict | None = None, late_delay: float = 0.0):
        # ws -> {"paused", "subscribed" (set), "topic_updates", "tu_include_schemas"}
        self.clients: dict = {}
        self.topics = list(BASE_TOPICS)          # mutable — may grow via --late-topic
        self.late_topic = late_topic
        self.late_delay = late_delay

    async def handler(self, websocket):
        client_id = id(websocket)
        self.clients[websocket] = {
            "paused": False,
            "subscribed": set(),
            "topic_updates": False,
            "tu_include_schemas": False,
        }
        print(f"[+] Client connected: {client_id}")
        try:
            async for message in websocket:
                if isinstance(message, (bytes, bytearray)):
                    continue
                await self.on_message(websocket, message)
        except websockets.exceptions.ConnectionClosed:
            pass
        finally:
            del self.clients[websocket]
            print(f"[-] Client disconnected: {client_id}")

    async def on_message(self, websocket, message):
        try:
            cmd = json.loads(message)
        except json.JSONDecodeError:
            return
        if not isinstance(cmd, dict):
            return

        command = cmd.get("command", "")
        state = self.clients[websocket]

        if command == "get_topics":
            include = bool(cmd.get("include_schemas", False))
            resp = {"status": "success",
                    "topics": [topic_entry(t, include) for t in self.topics]}
            await websocket.send(json.dumps(inject_response_fields(resp, cmd)))
            print(f"    get_topics (include_schemas={include}) → {len(self.topics)} topics")

        elif command == "subscribe":
            await self._handle_subscribe(websocket, cmd, state)

        elif command == "unsubscribe":
            requested = parse_topic_names(cmd.get("topics", []))
            removed = [n for n in requested if n in state["subscribed"]]
            for n in removed:
                state["subscribed"].discard(n)
            resp = {"status": "success", "removed": removed}
            await websocket.send(json.dumps(inject_response_fields(resp, cmd)))
            print(f"    unsubscribe → removed {removed}")

        elif command == "subscribe_topic_updates":
            state["topic_updates"] = True
            state["tu_include_schemas"] = bool(cmd.get("include_schemas", False))
            resp = {"status": "ok", "topic_updates": True}
            await websocket.send(json.dumps(inject_response_fields(resp, cmd)))
            print(f"    subscribe_topic_updates (include_schemas={state['tu_include_schemas']})")

        elif command == "unsubscribe_topic_updates":
            state["topic_updates"] = False
            resp = {"status": "ok", "topic_updates": False}
            await websocket.send(json.dumps(inject_response_fields(resp, cmd)))
            print("    unsubscribe_topic_updates")

        elif command == "heartbeat":
            resp = {"status": "ok"}
            await websocket.send(json.dumps(inject_response_fields(resp, cmd)))

        elif command == "pause":
            state["paused"] = True
            resp = {"status": "ok", "paused": True}
            await websocket.send(json.dumps(inject_response_fields(resp, cmd)))
            print("    paused")

        elif command == "resume":
            state["paused"] = False
            resp = {"status": "ok", "paused": False}
            await websocket.send(json.dumps(inject_response_fields(resp, cmd)))
            print("    resumed")

    async def _handle_subscribe(self, websocket, cmd, state):
        """ADDITIVE subscribe: merge newly-requested topics into the session set.
        Already-subscribed topics are a no-op (no schema echoed) — matching
        production's topics_to_add = requested − current."""
        requested = parse_topic_names(cmd.get("topics", []))
        known = {t["name"]: t for t in self.topics}
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
        await websocket.send(json.dumps(inject_response_fields(resp, cmd)))
        print(f"    subscribe → {resp['status']}; schemas={list(schemas)}; failures={[f['topic'] for f in failures]}")

    async def schedule_late_topic(self):
        """Advertise one extra topic `late_delay` seconds after startup and push a
        topics_changed notification to every opted-in session (exercises
        subscribe_topic_updates end-to-end for the static server)."""
        if not self.late_topic:
            return
        await asyncio.sleep(self.late_delay)
        self.topics.append(self.late_topic)
        print(f"[*] late topic advertised: {self.late_topic['name']}")
        for websocket, state in list(self.clients.items()):
            if not state["topic_updates"]:
                continue
            note = {
                "notification": "topics_changed",
                "added": [topic_entry(self.late_topic, state["tu_include_schemas"])],
                "removed": [],
                "protocol_version": PROTOCOL_VERSION,
            }
            try:
                await websocket.send(json.dumps(note))
            except websockets.exceptions.ConnectionClosed:
                pass

    async def emit_loop(self):
        """Send data frames to all subscribed, non-paused clients at ~10 Hz."""
        t = 0.0
        while True:
            await asyncio.sleep(0.1)
            t += 0.1

            if not self.clients:
                continue

            ts_ns = int(time.time() * 1e9)

            for websocket, state in list(self.clients.items()):
                if state["paused"]:
                    continue
                subscribed = state["subscribed"]
                if not subscribed:
                    continue

                messages = []
                for name in subscribed:
                    cdr = json.dumps({"value": value_for(name, t)}).encode()
                    messages.append((name, ts_ns, cdr))

                if messages:
                    frame = build_binary_frame(messages)
                    try:
                        await websocket.send(frame)
                        print(f"    → frame {len(frame)}b → {[m[0] for m in messages]}", end="\r")
                    except websockets.exceptions.ConnectionClosed:
                        pass
                    except Exception as exc:
                        print(f"\n[!] emit_loop unexpected error: {exc!r}")


def parse_late_topic(spec: str) -> tuple[dict, float]:
    """Parse a --late-topic NAME:SECONDS spec into (topic_entry, delay)."""
    name, sep, secs = spec.rpartition(":")
    if not sep or not name:
        raise argparse.ArgumentTypeError("expected NAME:SECONDS, e.g. /test/late:3")
    try:
        delay = float(secs)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid seconds in --late-topic: {secs!r}") from exc
    entry = {"name": name, "type": "Float64", "encoding": "json", "definition": ""}
    return entry, delay


async def main(host: str, port: int, late_topic, late_delay):
    server = PjBridgeServer(late_topic, late_delay)
    print(f"PJ Bridge test server listening on ws://{host}:{port}")
    print(f"Topics: {[t['name'] for t in server.topics]}")
    if late_topic:
        print(f"Late topic: {late_topic['name']} advertised after {late_delay}s")
    print("Ctrl+C to stop\n")

    async with websockets.serve(server.handler, host, port):
        await asyncio.gather(server.emit_loop(), server.schedule_late_topic())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="PJ Bridge test server")
    parser.add_argument("--host", default=HOST, help=f"Bind address (default: {HOST})")
    parser.add_argument("--port", type=int, default=PORT, help=f"Port (default: {PORT})")
    parser.add_argument("--late-topic", metavar="NAME:SECONDS", default=None,
                        help="Advertise NAME this many SECONDS after startup, pushing "
                             "topics_changed to opted-in sessions (e.g. /test/late:3)")
    args = parser.parse_args()

    late_topic, late_delay = (None, 0.0)
    if args.late_topic:
        late_topic, late_delay = parse_late_topic(args.late_topic)

    try:
        asyncio.run(main(args.host, args.port, late_topic, late_delay))
    except KeyboardInterrupt:
        print("\nStopped.")
