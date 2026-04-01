#!/usr/bin/env python3
"""
PJ Bridge test server — emits synthetic time series to pj_proto_app.

Implements the server side of the PJ Bridge WebSocket protocol so that
the data_stream_pj_bridge plugin can connect and receive data.

Usage:
    pip install websockets zstandard
    python3 pj_bridge_test_server.py [--port PORT] [--host HOST]

Protocol summary:
    Client → Server  JSON: {"command": "get_topics", ...}
    Server → Client  JSON: {"status": "success", "topics": [...]}
    Client → Server  JSON: {"command": "subscribe", "topics": [...]}
    Server → Client  JSON: {"status": "success", "schemas": {...}}
    Server → Client  Binary frames (see build_binary_frame)
    Client → Server  JSON: {"command": "heartbeat"} (ignored)
    Client → Server  JSON: {"command": "pause" / "resume"}
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

# Topics advertised to the connecting plugin.
# encoding="json" means CDR payload is raw JSON bytes — decoded by parser_json.
TOPICS = [
    {
        "name": "/test/sine",
        "type": "Float64",
        "schema_name": "Float64",
        "encoding": "json",
        "definition": "",
    },
    {
        "name": "/test/cosine",
        "type": "Float64",
        "schema_name": "Float64",
        "encoding": "json",
        "definition": "",
    },
    {
        "name": "/test/sawtooth",
        "type": "Float64",
        "schema_name": "Float64",
        "encoding": "json",
        "definition": "",
    },
]

# Magic: "PJRB" in little-endian = 0x42524A50
MAGIC = 0x42524A50


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


class PjBridgeServer:
    def __init__(self):
        self.clients: dict = {}  # ws -> {"paused": bool, "subscribed_topics": list}

    async def handler(self, websocket):
        client_id = id(websocket)
        self.clients[websocket] = {"paused": False, "subscribed_topics": []}
        print(f"[+] Client connected: {client_id}")
        try:
            async for message in websocket:
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

        command = cmd.get("command", "")

        if command == "get_topics":
            await websocket.send(json.dumps({"status": "success", "topics": TOPICS}))
            print(f"    get_topics → sent {len(TOPICS)} topics")

        elif command == "subscribe":
            requested = cmd.get("topics", [])
            schemas = {}
            for topic_name in requested:
                for t in TOPICS:
                    if t["name"] == topic_name:
                        schemas[topic_name] = {
                            "encoding": t["encoding"],
                            "definition": t["definition"],
                        }
                        break
            self.clients[websocket]["subscribed_topics"] = requested
            response = json.dumps({"status": "success", "schemas": schemas})
            await websocket.send(response)
            print(f"    subscribe → {requested}")
            print(f"    schemas sent → {schemas}")

        elif command == "pause":
            self.clients[websocket]["paused"] = True
            print("    paused")

        elif command == "resume":
            self.clients[websocket]["paused"] = False
            print("    resumed")

        elif command == "heartbeat":
            pass  # heartbeat acknowledged silently

    async def emit_loop(self):
        """Send data frames to all subscribed, non-paused clients at ~10 Hz."""
        t = 0.0
        while True:
            await asyncio.sleep(0.1)
            t += 0.1

            if not self.clients:
                continue

            ts_ns = int(time.time() * 1e9)

            values = {
                "/test/sine": math.sin(t),
                "/test/cosine": math.cos(t),
                "/test/sawtooth": (t % (2 * math.pi)) / (2 * math.pi),
            }

            for websocket, state in list(self.clients.items()):
                if state["paused"]:
                    continue
                subscribed = state["subscribed_topics"]
                if not subscribed:
                    continue

                messages = []
                for topic_name in subscribed:
                    if topic_name in values:
                        cdr = json.dumps({"value": values[topic_name]}).encode()
                        messages.append((topic_name, ts_ns, cdr))

                if messages:
                    frame = build_binary_frame(messages)
                    try:
                        await websocket.send(frame)
                        print(f"    → frame {len(frame)}b → {[m[0] for m in messages]}", end="\r")
                    except websockets.exceptions.ConnectionClosed:
                        pass
                    except Exception as exc:
                        print(f"\n[!] emit_loop unexpected error: {exc!r}")


async def main(host: str, port: int):
    server = PjBridgeServer()
    print(f"PJ Bridge test server listening on ws://{host}:{port}")
    print(f"Topics: {[t['name'] for t in TOPICS]}")
    print("Ctrl+C to stop\n")

    async with websockets.serve(server.handler, host, port):
        await server.emit_loop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="PJ Bridge test server")
    parser.add_argument("--host", default=HOST, help=f"Bind address (default: {HOST})")
    parser.add_argument("--port", type=int, default=PORT, help=f"Port (default: {PORT})")
    args = parser.parse_args()

    try:
        asyncio.run(main(args.host, args.port))
    except KeyboardInterrupt:
        print("\nStopped.")
