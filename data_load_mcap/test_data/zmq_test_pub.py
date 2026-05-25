#!/usr/bin/env python3
"""
ZMQ publisher for PlotJuggler 4 — tests timestamp handling.

Wire format (3-frame multipart):
  Frame 1: topic name   (e.g. "sensors")
  Frame 2: JSON payload (e.g. {"sin": 1.23, "cos": -0.45})
  Frame 3: timestamp    (seconds since epoch as ASCII text, e.g. "1716800000.123")

Frame 3 lets PJ4 use the payload timestamp instead of receive time.
Omit Frame 3 to use receive time instead (set USE_PAYLOAD_TS = False).

Install:  pip install pyzmq
Run:      python3 scripts/zmq_test_pub.py

In PJ4 ZMQ dialog:
  Address:   localhost
  Port:      9872
  Transport: tcp://
  Mode:      connect   <- plugin connects to this publisher
  Encoding:  json
"""

import json
import math
import random
import time

import zmq

# ── Config ────────────────────────────────────────────────────────────────────
PORT = 9872
RATE_HZ = 20
DT = 1.0 / RATE_HZ

# True  → send payload timestamp in Frame 3 (tests ScalarRecord::ts override)
# False → plugin uses receive time (tests transport timestamp path)
USE_PAYLOAD_TS = True
# ──────────────────────────────────────────────────────────────────────────────


def publish(socket, topic: str, payload: dict, ts_sec: float | None = None):
    frames = [
        topic.encode(),
        json.dumps(payload).encode(),
    ]
    if ts_sec is not None:
        frames.append(f"{ts_sec:.6f}".encode())
    socket.send_multipart(frames)


def main():
    ctx = zmq.Context()
    socket = ctx.socket(zmq.PUB)
    socket.bind(f"tcp://*:{PORT}")

    # ZMQ PUB needs a brief warmup so subscribers receive the first messages.
    time.sleep(0.3)

    ts_mode = "payload timestamp" if USE_PAYLOAD_TS else "receive time"
    print(f"ZMQ PUB on tcp://*:{PORT} at {RATE_HZ} Hz  [{ts_mode}]")
    print("Topics: sensors  imu  (Ctrl+C to stop)\n")

    t = 0.0
    start_wall = time.time()

    while True:
        wall = time.time()
        ts = wall if USE_PAYLOAD_TS else None

        sensors = {
            "sin":  round(5.0 * math.sin(2 * math.pi * 0.5 * t), 4),
            "cos":  round(5.0 * math.cos(2 * math.pi * 0.5 * t), 4),
            "ramp": round((t % 5.0) * 2.0,                        4),
        }
        imu = {
            "accel_x": round(math.sin(2 * math.pi * 1.0 * t) + random.uniform(-0.1, 0.1), 4),
            "accel_y": round(math.cos(2 * math.pi * 0.7 * t) + random.uniform(-0.1, 0.1), 4),
            "accel_z": round(9.81 + 0.2 * math.sin(2 * math.pi * 2.0 * t),                4),
        }

        publish(socket, "sensors", sensors, ts)
        publish(socket, "imu",     imu,     ts)

        elapsed = wall - start_wall
        print(f"\r  t={elapsed:6.1f}s  sin={sensors['sin']:+.2f}  ax={imu['accel_x']:+.2f}", end="", flush=True)

        t += DT
        time.sleep(DT)


if __name__ == "__main__":
    main()
