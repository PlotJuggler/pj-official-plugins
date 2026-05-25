#!/usr/bin/env python3
"""
Generate MCAP test files for the data_load_mcap verification plan.

See: docs/claude_reports/plugins_analysis/data_load_mcap_verification.md

Files generated:
  test_publish_vs_log_time.mcap  -- Test 1: publish_time != log_time (offset 500ms)
  test_embedded_timestamp.mcap   -- Test 2: Header.stamp differs from MCAP timestamp (+2s)
  test_large_arrays.mcap         -- Test 3: float64[1000] arrays for clamp/skip testing
  test_empty_channel.mcap        -- Test 6: one channel with 0 messages
  test_unsupported_encoding.mcap -- Test 7: channel with encoding "protobuf" (no parser)
  test_bad_schema.mcap           -- Test 8: channel with malformed schema definition
  test_large_file.mcap           -- Test 9: 50000 messages for cancellation testing

Requires: pip install mcap
"""

import math
import struct
from pathlib import Path

from mcap.writer import Writer


# ---------------------------------------------------------------------------
# CDR helpers
# ---------------------------------------------------------------------------

def cdr_float64(value: float) -> bytes:
    """Single float64 field as ROS2 CDR LE (with encapsulation header)."""
    return struct.pack("<4xd", value)  # 4-byte encapsulation + double


def cdr_header_float64(sec: int, nanosec: int, frame_id: str, value: float) -> bytes:
    """Header + float64 as ROS2 CDR LE.

    Header layout: int32 sec, uint32 nanosec, string frame_id
    """
    frame_bytes = frame_id.encode() + b"\x00"
    buf = bytearray(b"\x00\x01\x00\x00")  # encapsulation
    buf.extend(struct.pack("<iI", sec, nanosec))
    buf.extend(struct.pack("<I", len(frame_bytes)))
    buf.extend(frame_bytes)
    # Align to 8 bytes for float64
    while len(buf) % 8 != 0:
        buf.append(0)
    buf.extend(struct.pack("<d", value))
    return bytes(buf)


def cdr_float64_fixed_array(values: list) -> bytes:
    """Fixed-size float64 array as ROS2 CDR LE."""
    buf = bytearray(b"\x00\x01\x00\x00")  # encapsulation
    for v in values:
        buf.extend(struct.pack("<d", v))
    return bytes(buf)


SCHEMA_FLOAT64 = b"float64 data\n"

SCHEMA_NESTED = (
    b"Header  header\n"
    b"float64 value\n"
    b"================\n"
    b"MSG: std_msgs/Header\n"
    b"int32  sec\n"
    b"uint32 nanosec\n"
    b"string frame_id\n"
)

OUT = Path(__file__).parent


# ---------------------------------------------------------------------------
# Test 1: Publish time vs log time
# ---------------------------------------------------------------------------

def gen_publish_vs_log_time():
    path = OUT / "test_publish_vs_log_time.mcap"
    n_msgs = 50
    dt_ns = 100_000_000  # 100ms = 10 Hz
    offset_ns = 500_000_000  # 500ms offset between publish and log

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="ros2", library="pj-verification")

        schema_id = w.register_schema(
            name="std_msgs/Float64", encoding="ros2msg", data=SCHEMA_FLOAT64,
        )
        ch = w.register_channel(
            topic="/sensor/value", message_encoding="cdr", schema_id=schema_id,
        )

        for i in range(n_msgs):
            publish_time = i * dt_ns
            log_time = publish_time + offset_ns  # log_time is 500ms later
            w.add_message(
                channel_id=ch,
                log_time=log_time,
                publish_time=publish_time,
                data=cdr_float64(math.sin(i * 0.2)),
            )

        w.finish()
    print(f"[OK] {path.name:45s} {path.stat().st_size:>8} bytes")


# ---------------------------------------------------------------------------
# Test 2: Embedded timestamp
# ---------------------------------------------------------------------------

def gen_embedded_timestamp():
    path = OUT / "test_embedded_timestamp.mcap"
    n_msgs = 50
    dt_ns = 100_000_000  # 100ms
    stamp_offset_s = 2  # Header.stamp is 2 seconds ahead of MCAP timestamp

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="ros2", library="pj-verification")

        schema_id = w.register_schema(
            name="test_msgs/StampedValue", encoding="ros2msg", data=SCHEMA_NESTED,
        )
        ch = w.register_channel(
            topic="/stamped/value", message_encoding="cdr", schema_id=schema_id,
        )

        for i in range(n_msgs):
            mcap_ts = i * dt_ns
            header_sec = i // 10 + stamp_offset_s
            header_nsec = (i % 10) * 100_000_000
            value = 20.0 + i * 0.1

            w.add_message(
                channel_id=ch,
                log_time=mcap_ts,
                publish_time=mcap_ts,
                data=cdr_header_float64(header_sec, header_nsec, "sensor", value),
            )

        w.finish()
    print(f"[OK] {path.name:45s} {path.stat().st_size:>8} bytes")


# ---------------------------------------------------------------------------
# Test 3: Large arrays
# ---------------------------------------------------------------------------

def gen_large_arrays():
    path = OUT / "test_large_arrays.mcap"
    n_msgs = 30
    array_size = 1000
    dt_ns = 100_000_000

    schema_text = f"float64[{array_size}] data\n".encode()

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="ros2", library="pj-verification")

        schema_id = w.register_schema(
            name="test_msgs/BigArray", encoding="ros2msg", data=schema_text,
        )
        ch = w.register_channel(
            topic="/big_array", message_encoding="cdr", schema_id=schema_id,
        )

        for i in range(n_msgs):
            ts = i * dt_ns
            values = [math.sin(i * 0.1 + k * 0.01) for k in range(array_size)]
            w.add_message(
                channel_id=ch,
                log_time=ts,
                publish_time=ts,
                data=cdr_float64_fixed_array(values),
            )

        w.finish()
    print(f"[OK] {path.name:45s} {path.stat().st_size:>8} bytes")


# ---------------------------------------------------------------------------
# Test 6: Empty channel
# ---------------------------------------------------------------------------

def gen_empty_channel():
    path = OUT / "test_empty_channel.mcap"
    n_msgs = 50
    dt_ns = 100_000_000

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="ros2", library="pj-verification")

        schema_id = w.register_schema(
            name="std_msgs/Float64", encoding="ros2msg", data=SCHEMA_FLOAT64,
        )

        ch_active1 = w.register_channel(
            topic="/active/data", message_encoding="cdr", schema_id=schema_id,
        )
        ch_active2 = w.register_channel(
            topic="/active/status", message_encoding="cdr", schema_id=schema_id,
        )
        # Registered but never receives messages
        _ = w.register_channel(
            topic="/inactive/placeholder", message_encoding="cdr", schema_id=schema_id,
        )

        for i in range(n_msgs):
            ts = i * dt_ns
            w.add_message(
                channel_id=ch_active1, log_time=ts, publish_time=ts,
                data=cdr_float64(math.sin(i * 0.2)),
            )
            w.add_message(
                channel_id=ch_active2, log_time=ts, publish_time=ts,
                data=cdr_float64(i * 0.5),
            )

        w.finish()
    print(f"[OK] {path.name:45s} {path.stat().st_size:>8} bytes")


# ---------------------------------------------------------------------------
# Test 7: Unsupported encoding
# ---------------------------------------------------------------------------

def gen_unsupported_encoding():
    path = OUT / "test_unsupported_encoding.mcap"
    n_msgs = 10
    dt_ns = 100_000_000

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="ros2", library="pj-verification")

        # Good channel: CDR + ros2msg (supported)
        schema_good = w.register_schema(
            name="std_msgs/Float64", encoding="ros2msg", data=SCHEMA_FLOAT64,
        )
        ch_good = w.register_channel(
            topic="/ros2/data", message_encoding="cdr", schema_id=schema_good,
        )

        # Bad channel: protobuf encoding (no parser registered)
        schema_bad = w.register_schema(
            name="test.SensorReading", encoding="proto",
            data=b'syntax = "proto3"; message SensorReading { double value = 1; }',
        )
        ch_bad = w.register_channel(
            topic="/proto/data", message_encoding="protobuf", schema_id=schema_bad,
        )

        for i in range(n_msgs):
            ts = i * dt_ns
            w.add_message(
                channel_id=ch_good, log_time=ts, publish_time=ts,
                data=cdr_float64(i * 1.1),
            )
            # Write dummy protobuf bytes (doesn't need to be valid — parser won't exist)
            w.add_message(
                channel_id=ch_bad, log_time=ts, publish_time=ts,
                data=struct.pack("<d", i * 2.2),
            )

        w.finish()
    print(f"[OK] {path.name:45s} {path.stat().st_size:>8} bytes")


# ---------------------------------------------------------------------------
# Test 8: Bad schema
# ---------------------------------------------------------------------------

def gen_bad_schema():
    path = OUT / "test_bad_schema.mcap"
    n_msgs = 10
    dt_ns = 100_000_000

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="ros2", library="pj-verification")

        # Good schema
        schema_good = w.register_schema(
            name="std_msgs/Float64", encoding="ros2msg", data=SCHEMA_FLOAT64,
        )
        ch_good = w.register_channel(
            topic="/good/data", message_encoding="cdr", schema_id=schema_good,
        )

        # Bad schema: encoding says ros2msg but content is garbage
        schema_bad = w.register_schema(
            name="broken_msgs/Garbage", encoding="ros2msg",
            data=b"THIS IS NOT A VALID ROS MSG DEFINITION }{][",
        )
        ch_bad = w.register_channel(
            topic="/bad/data", message_encoding="cdr", schema_id=schema_bad,
        )

        for i in range(n_msgs):
            ts = i * dt_ns
            w.add_message(
                channel_id=ch_good, log_time=ts, publish_time=ts,
                data=cdr_float64(i * 3.3),
            )
            w.add_message(
                channel_id=ch_bad, log_time=ts, publish_time=ts,
                data=cdr_float64(i * 4.4),  # valid CDR but schema is broken
            )

        w.finish()
    print(f"[OK] {path.name:45s} {path.stat().st_size:>8} bytes")


# ---------------------------------------------------------------------------
# Test 9: Large file (50k messages)
# ---------------------------------------------------------------------------

def gen_large_file():
    path = OUT / "test_large_file.mcap"
    n_msgs = 50_000
    dt_ns = 20_000  # 20 us apart (very fast, just for volume)

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="ros2", library="pj-verification")

        schema_id = w.register_schema(
            name="std_msgs/Float64", encoding="ros2msg", data=SCHEMA_FLOAT64,
        )
        ch = w.register_channel(
            topic="/bulk/data", message_encoding="cdr", schema_id=schema_id,
        )

        for i in range(n_msgs):
            ts = i * dt_ns
            w.add_message(
                channel_id=ch, log_time=ts, publish_time=ts,
                data=cdr_float64(math.sin(i * 0.001)),
            )

        w.finish()
    print(f"[OK] {path.name:45s} {path.stat().st_size:>8} bytes")


# ---------------------------------------------------------------------------
# JSON embedded timestamp with field name "timestamp" (standard)
# ---------------------------------------------------------------------------

def gen_json_embedded_ts():
    """JSON messages with embedded 'timestamp' field ahead of MCAP log_time by 5 s.

    Use this file to verify timestamp_field_name="timestamp" config in the JSON parser.
    Three channels:
      /sensor/scalar  -- flat object: {timestamp, temperature, pressure}
      /sensor/nested  -- nested object: {timestamp, accel{x,y,z}, speed}
      /sensor/array   -- root array of joint objects (no embedded ts in elements)
    """
    import math as _math
    path = OUT / "test_json_embedded_ts.mcap"
    n_msgs = 50
    dt_ns = 100_000_000   # 100 ms steps
    log_start_ns = 5_000_000_000  # log_time starts at t=5 s
    ts_start_s = 0.0      # embedded timestamp starts at 0.0 s

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="", library="pj-verification")

        schema_id = w.register_schema(name="json", encoding="jsonschema", data=b"{}")
        ch_scalar = w.register_channel(topic="/sensor/scalar", message_encoding="json", schema_id=schema_id)
        ch_nested = w.register_channel(topic="/sensor/nested", message_encoding="json", schema_id=schema_id)
        ch_array  = w.register_channel(topic="/sensor/array",  message_encoding="json", schema_id=schema_id)

        joints = ["shoulder", "elbow", "wrist"]
        joint_vel = [0.1, 0.2, 0.3]

        for i in range(n_msgs):
            log_ts = log_start_ns + i * dt_ns
            t = ts_start_s + i * 0.1

            scalar_msg = {
                "timestamp": round(t, 4),
                "temperature": round(20.0 + _math.log1p(i) * 0.2, 4),
                "pressure": round(1014.0 - i * 0.018 + _math.sin(i * 0.3) * 0.05, 4),
            }
            nested_msg = {
                "timestamp": round(t, 4),
                "accel": {"x": round(_math.sin(t), 4), "y": round(_math.cos(t), 4), "z": round(9.81 + _math.sin(t * 0.5) * 0.05, 4)},
                "speed": round(abs(_math.sin(t)), 4),
            }
            array_msg = [
                {"joint": j, "angle": round(_math.sin(t + k * 0.5), 4), "velocity": round(v, 4)}
                for k, (j, v) in enumerate(zip(joints, joint_vel))
            ]

            import json as _json
            for ch, msg in [(ch_scalar, scalar_msg), (ch_nested, nested_msg), (ch_array, array_msg)]:
                data = _json.dumps(msg).encode()
                w.add_message(channel_id=ch, log_time=log_ts, publish_time=log_ts, data=data)

        w.finish()
    print(f"[OK] {path.name:45s} {path.stat().st_size:>8} bytes")


# ---------------------------------------------------------------------------
# JSON embedded timestamp with custom field name "ts"
# ---------------------------------------------------------------------------

def gen_json_embedded_ts_custom():
    """Identical to gen_json_embedded_ts but uses 'ts' as the timestamp field.

    Use this file to verify that timestamp_field_name="ts" config works, i.e.
    the field name is not hardcoded to "timestamp". The MCAP log_time is 5 s
    ahead of the embedded 'ts' values, so if the parser reads 'ts' correctly
    the resulting series timestamps will start at 0 s, not 5 s.
    Three channels:
      /sensor/scalar  -- flat object: {ts, temperature, pressure}
      /sensor/nested  -- nested object: {ts, accel{x,y,z}, speed}
      /sensor/array   -- root array of joint objects (no embedded ts in elements)
    """
    import math as _math
    import json as _json
    path = OUT / "test_json_embedded_ts_custom.mcap"
    n_msgs = 50
    dt_ns = 100_000_000
    log_start_ns = 5_000_000_000
    ts_start_s = 0.0

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="", library="pj-verification")

        schema_id = w.register_schema(name="json", encoding="jsonschema", data=b"{}")
        ch_scalar = w.register_channel(topic="/sensor/scalar", message_encoding="json", schema_id=schema_id)
        ch_nested = w.register_channel(topic="/sensor/nested", message_encoding="json", schema_id=schema_id)
        ch_array  = w.register_channel(topic="/sensor/array",  message_encoding="json", schema_id=schema_id)

        joints = ["shoulder", "elbow", "wrist"]
        joint_vel = [0.1, 0.2, 0.3]

        for i in range(n_msgs):
            log_ts = log_start_ns + i * dt_ns
            t = ts_start_s + i * 0.1

            scalar_msg = {
                "ts": round(t, 4),
                "temperature": round(20.0 + _math.log1p(i) * 0.2, 4),
                "pressure": round(1014.0 - i * 0.018 + _math.sin(i * 0.3) * 0.05, 4),
            }
            nested_msg = {
                "ts": round(t, 4),
                "accel": {"x": round(_math.sin(t), 4), "y": round(_math.cos(t), 4), "z": round(9.81 + _math.sin(t * 0.5) * 0.05, 4)},
                "speed": round(abs(_math.sin(t)), 4),
            }
            array_msg = [
                {"joint": j, "angle": round(_math.sin(t + k * 0.5), 4), "velocity": round(v, 4)}
                for k, (j, v) in enumerate(zip(joints, joint_vel))
            ]

            for ch, msg in [(ch_scalar, scalar_msg), (ch_nested, nested_msg), (ch_array, array_msg)]:
                data = _json.dumps(msg).encode()
                w.add_message(channel_id=ch, log_time=log_ts, publish_time=log_ts, data=data)

        w.finish()
    print(f"[OK] {path.name:45s} {path.stat().st_size:>8} bytes")


# ---------------------------------------------------------------------------

if __name__ == "__main__":
    print("Generating verification MCAPs...\n")

    gen_publish_vs_log_time()
    gen_embedded_timestamp()
    gen_large_arrays()
    gen_empty_channel()
    gen_unsupported_encoding()
    gen_bad_schema()
    gen_large_file()
    gen_json_embedded_ts()
    gen_json_embedded_ts_custom()

    print("\nDone. See data_load_mcap_verification.md for test procedures.")
