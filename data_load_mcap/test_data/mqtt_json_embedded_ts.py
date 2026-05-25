#!/usr/bin/env python3
"""
MQTT publisher — JSON with embedded timestamp field.

Each JSON payload contains a "timestamp" field starting at 0.0 s.
The MQTT receive time is Unix epoch (~1.78e9 s).

Verify embedded timestamp works:
  ☑  Use embedded timestamp  →  X axis  0..∞ s  (relative, clean)
  ☐  Disabled               →  X axis ~1.78e9 s (Unix epoch)

Requires:
  pip install paho-mqtt
  sudo systemctl start mosquitto

Run:
  python3 mqtt_json_embedded_ts.py

In PJ4 MQTT dialog:
  Broker: localhost:1883  |  Encoding: json
  Configure parser → ☑ Use embedded timestamp | field: timestamp
"""

import json
import math
import random
import time

import paho.mqtt.client as mqtt

HOST    = "localhost"
PORT    = 1883
RATE_HZ = 20
DT      = 1.0 / RATE_HZ


def main():
    client = mqtt.Client()
    client.connect(HOST, PORT)
    client.loop_start()

    print(f"MQTT PUB  {HOST}:{PORT}  at {RATE_HZ} Hz")
    print("Topics: pj/sensors  pj/imu")
    print()
    print("In PJ4 → MQTT dialog → Configure parser:")
    print("  ☑ Use embedded timestamp | field: timestamp")
    print("  → X axis starts at 0 s (not Unix epoch ~1.78e9 s)")
    print()
    print("Ctrl+C to stop\n")

    t = 0.0

    while True:
        sensors = {
            "timestamp": round(t, 4),
            "sin":       round(5.0 * math.sin(2 * math.pi * 0.5 * t), 4),
            "cos":       round(5.0 * math.cos(2 * math.pi * 0.5 * t), 4),
            "ramp":      round((t % 5.0) * 2.0,                        4),
        }
        imu = {
            "timestamp": round(t, 4),
            "accel_x":   round(math.sin(2 * math.pi * 1.0 * t) + random.uniform(-0.1, 0.1), 4),
            "accel_y":   round(math.cos(2 * math.pi * 0.7 * t) + random.uniform(-0.1, 0.1), 4),
            "accel_z":   round(9.81 + 0.2 * math.sin(2 * math.pi * 2.0 * t),                4),
        }

        client.publish("pj/sensors", json.dumps(sensors))
        client.publish("pj/imu",     json.dumps(imu))

        print(f"\r  t={t:7.2f}s  sin={sensors['sin']:+.2f}  ax={imu['accel_x']:+.2f}", end="", flush=True)

        t += DT
        time.sleep(DT)


if __name__ == "__main__":
    main()
