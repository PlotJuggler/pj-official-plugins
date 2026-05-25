#!/usr/bin/env python3
"""
ZMQ publisher — JSON with embedded timestamp field.

Each JSON payload contains a "timestamp" field that starts at 0.0 s
and increments at 20 Hz.  The ZMQ receive time is Unix epoch (~1.7e9 s).

This makes the embedded-timestamp feature easy to verify:
  - With  "Use embedded timestamp": X axis  0..∞ s  (relative, clean)
  - Without it:                      X axis ~1.7e9 s (Unix epoch, huge)

Wire format  (2-frame multipart, no timestamp frame):
  Frame 1: topic  (e.g. b"sensors")
  Frame 2: JSON   (e.g. b'{"timestamp":0.05,"sin":1.23,...}')

NOTE: we deliberately do NOT send Frame 3 so the plugin's receive-time
path is exercised when embedded timestamp is disabled.

Install:  pip install pyzmq
Run:      python3 zmq_json_embedded_ts.py

In PJ4 ZMQ dialog:
  Address: localhost | Port: 9872 | Mode: connect | Encoding: json
Then in the "Configure parser" section:
  ☑  Use embedded timestamp field
  Field name: timestamp           (or leave blank)
"""

import json
import math
import random
import time

import zmq

PORT    = 9872
RATE_HZ = 20
DT      = 1.0 / RATE_HZ


def main():
    ctx    = zmq.Context()
    socket = ctx.socket(zmq.PUB)
    socket.bind(f"tcp://*:{PORT}")
    time.sleep(0.3)   # ZMQ slow-joiner warmup

    print(f"ZMQ PUB  tcp://*:{PORT}  at {RATE_HZ} Hz")
    print("Topics: sensors  imu")
    print()
    print("In PJ4 → ZMQ dialog → Configure parser:")
    print("  ☑ Use embedded timestamp | field: timestamp")
    print("  → X axis starts at 0 s (not Unix epoch)")
    print()
    print("Ctrl+C to stop\n")

    embedded_t = 0.0   # starts at 0, increments cleanly — NOT Unix epoch

    while True:
        sensors = {
            "timestamp": round(embedded_t, 4),
            "sin":       round(5.0 * math.sin(2 * math.pi * 0.5 * embedded_t), 4),
            "cos":       round(5.0 * math.cos(2 * math.pi * 0.5 * embedded_t), 4),
            "ramp":      round((embedded_t % 5.0) * 2.0,                        4),
        }
        imu = {
            "timestamp": round(embedded_t, 4),
            "accel_x":   round(math.sin(2 * math.pi * 1.0 * embedded_t) + random.uniform(-0.1, 0.1), 4),
            "accel_y":   round(math.cos(2 * math.pi * 0.7 * embedded_t) + random.uniform(-0.1, 0.1), 4),
            "accel_z":   round(9.81 + 0.2 * math.sin(2 * math.pi * 2.0 * embedded_t),                4),
        }

        # 2-frame multipart: topic + JSON payload (no timestamp frame)
        socket.send_multipart([b"sensors", json.dumps(sensors).encode()])
        socket.send_multipart([b"imu",     json.dumps(imu).encode()])

        print(f"\r  t={embedded_t:7.2f}s  sin={sensors['sin']:+.2f}  ax={imu['accel_x']:+.2f}", end="", flush=True)

        embedded_t += DT
        time.sleep(DT)


if __name__ == "__main__":
    main()
