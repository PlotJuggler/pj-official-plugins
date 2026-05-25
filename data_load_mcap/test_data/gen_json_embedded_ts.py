#!/usr/bin/env python3
"""
Generate test_json_embedded_ts.mcap:
  - 200 messages per channel (sensors + imu), at 20 Hz → 10 s of data
  - Each JSON message contains a "timestamp" field (seconds, starts at 0.0)
  - MCAP log_time starts at t=5 s (Unix epoch offset to make the mismatch obvious)
  - Two channels:
      /sensors  ->  { "timestamp": t, "sin": ..., "cos": ..., "ramp": ... }
      /imu      ->  { "timestamp": t, "accel_x": ..., "accel_y": ..., "accel_z": ... }

How to open in PJ4:
  1. File → Open → test_json_embedded_ts.mcap
  2. In the MCAP dialog: encoding = json  (auto-detected)
  3. Click the "Configure parser" section that appears for JSON:
       ☑  Use embedded timestamp field
       Field name: timestamp          (or leave blank — parser tries "timestamp" by default)
  4. Accept
  Expected result:
    - X axis starts at t = 0 s  (embedded timestamp)
    - Without the option: X axis starts at t = 5 s  (MCAP log_time)

Install:  pip install mcap
Run:      python3 gen_json_embedded_ts.py
"""

import json
import math
import random
from pathlib import Path

from mcap.writer import Writer

OUT = Path(__file__).parent
RATE_HZ = 20
N_SAMPLES = 200                             # 10 s of data
MCAP_START_NS = 5_000_000_000              # log_time offset: 5 s in ns
NS_PER_SAMPLE = 1_000_000_000 // RATE_HZ  # 50 ms per sample


def sensors_msg(t: float) -> dict:
    return {
        "timestamp":  round(t, 6),
        "sin":        round(5.0 * math.sin(2 * math.pi * 0.5 * t), 4),
        "cos":        round(5.0 * math.cos(2 * math.pi * 0.5 * t), 4),
        "ramp":       round((t % 5.0) * 2.0,                        4),
        "square":     3.0 if math.sin(2 * math.pi * 0.5 * t) >= 0 else -3.0,
    }


def imu_msg(t: float) -> dict:
    return {
        "timestamp": round(t, 6),
        "accel_x":   round(math.sin(2 * math.pi * 1.0 * t) + random.uniform(-0.05, 0.05), 4),
        "accel_y":   round(math.cos(2 * math.pi * 0.7 * t) + random.uniform(-0.05, 0.05), 4),
        "accel_z":   round(9.81 + 0.2 * math.sin(2 * math.pi * 2.0 * t),                  4),
        "gyro_z":    round(0.1 * math.sin(2 * math.pi * 0.3 * t),                          4),
    }


def main():
    out_path = OUT / "test_json_embedded_ts.mcap"
    with open(out_path, "wb") as f:
        writer = Writer(f)
        writer.start(profile="", library="gen_json_embedded_ts")

        # JSON schema: empty (self-describing format)
        schema_id = writer.register_schema(
            name="json",
            encoding="jsonschema",
            data=b"{}",
        )
        ch_sensors = writer.register_channel(
            topic="/sensors",
            message_encoding="json",
            schema_id=schema_id,
        )
        ch_imu = writer.register_channel(
            topic="/imu",
            message_encoding="json",
            schema_id=schema_id,
        )

        for i in range(N_SAMPLES):
            t = i / RATE_HZ                               # embedded time: 0.0 .. 9.95 s
            log_time = MCAP_START_NS + i * NS_PER_SAMPLE  # log_time: 5.0 .. 14.95 s

            writer.add_message(
                channel_id=ch_sensors,
                log_time=log_time,
                publish_time=log_time,
                sequence=i,
                data=json.dumps(sensors_msg(t)).encode(),
            )
            writer.add_message(
                channel_id=ch_imu,
                log_time=log_time,
                publish_time=log_time,
                sequence=i,
                data=json.dumps(imu_msg(t)).encode(),
            )

        writer.finish()

    print(f"Written {out_path}  ({N_SAMPLES} messages × 2 channels)")
    print()
    print("Open in PJ4:")
    print("  File → Open → test_json_embedded_ts.mcap")
    print("  Encoding: json")
    print("  ☑ Use embedded timestamp  |  Field: timestamp")
    print("  → X axis: 0..10 s  (not 5..15 s)")


if __name__ == "__main__":
    main()
