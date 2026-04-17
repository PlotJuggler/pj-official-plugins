#!/usr/bin/env python3
"""
UDP test client for the PlotJuggler UDP Server plugin.

Sends JSON data at 20 Hz (sine and cosine waves) to the configured address/port.
Mirrors the original test client from PlotJuggler 3.x.

Usage:
    python udp_client.py [--address ADDRESS] [--port PORT]

Defaults: 127.0.0.1:9870
"""

import argparse
import json
import math
import socket
import time


def main():
    parser = argparse.ArgumentParser(description="UDP test client for PlotJuggler")
    parser.add_argument("--address", default="127.0.0.1", help="Target address (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9870, help="Target port (default: 9870)")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.address, args.port)

    print(f"Sending JSON data to {args.address}:{args.port} at 20 Hz — Ctrl+C to stop")

    t = 0.0
    try:
        while True:
            data = {
                "timestamp": t,
                "test_data": {
                    "cos": math.cos(t),
                    "sin": math.sin(t),
                    "sin2": math.sin(2 * t),
                    "cos3": math.cos(3 * t),
                },
            }
            sock.sendto(json.dumps(data).encode(), target)
            t += 0.05
            time.sleep(0.05)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
