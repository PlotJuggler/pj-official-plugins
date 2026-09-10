#!/usr/bin/env python3
"""
Generate MCAP test files for the data_load_mcap verification plan.

Each generated file targets one verification case; the case each exercises is
described inline with its generator function below.

Files generated:
  test_publish_vs_log_time.mcap  -- Test 1: publish_time != log_time (offset 500ms)
  test_embedded_timestamp.mcap   -- Test 2: Header.stamp differs from MCAP timestamp (+2s)
  test_dialog_whitelist.mcap     -- Test: always-include whitelist (tf2_msgs/TFMessage +
                                     foxglove.FrameTransform matches, empty channel, near
                                     miss, and ordinary channel)
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

SCHEMA_TF2_MSGS = (
    b"geometry_msgs/TransformStamped[] transforms\n"
    b"================\n"
    b"MSG: geometry_msgs/TransformStamped\n"
    b"Header header\n"
    b"string child_frame_id\n"
    b"Transform transform\n"
    b"================\n"
    b"MSG: std_msgs/Header\n"
    b"int32  sec\n"
    b"uint32 nanosec\n"
    b"string frame_id\n"
    b"================\n"
    b"MSG: geometry_msgs/Transform\n"
    b"Vector3 translation\n"
    b"Quaternion rotation\n"
)

SCHEMA_FOXGLOVE_FRAME_TRANSFORM_PROTO = (
    b'syntax = "proto3";\n'
    b"package foxglove;\n"
    b"message FrameTransform {\n"
    b"  string parent_frame_id = 2;\n"
    b"  string child_frame_id = 3;\n"
    b"}\n"
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
# Test: always-include whitelist (schema + encoding based, not topic name)
# ---------------------------------------------------------------------------

def gen_dialog_whitelist():
    path = OUT / "test_dialog_whitelist.mcap"
    n_msgs = 5
    dt_ns = 100_000_000

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="ros2", library="pj-verification")

        # 1. ROS 2 /tf -- whitelisted (schema tf2_msgs/msg/TFMessage, encoding cdr).
        schema_tf2 = w.register_schema(
            name="tf2_msgs/msg/TFMessage", encoding="ros2msg", data=SCHEMA_TF2_MSGS,
        )
        ch_tf = w.register_channel(topic="/tf", message_encoding="cdr", schema_id=schema_tf2)

        # 2. Foxglove transform under a non-conventional topic name -- whitelisted
        #    (schema foxglove.FrameTransform, encoding protobuf). Proves the match
        #    is on message type, not on topic naming: real Foxglove-recorded files
        #    use topic names like plain "tf" (no leading slash) or arbitrary names.
        schema_foxglove = w.register_schema(
            name="foxglove.FrameTransform", encoding="protobuf",
            data=SCHEMA_FOXGLOVE_FRAME_TRANSFORM_PROTO,
        )
        ch_foxglove = w.register_channel(
            topic="transforms", message_encoding="protobuf", schema_id=schema_foxglove,
        )

        # 3. /tf_static -- same whitelisted schema/encoding, but zero messages:
        #    must NOT be force-included (nothing to load).
        _ = w.register_channel(topic="/tf_static", message_encoding="cdr", schema_id=schema_tf2)

        # 4. Near miss: right schema name, wrong encoding -- must NOT be force-included.
        schema_tf2_json = w.register_schema(
            name="tf2_msgs/msg/TFMessage", encoding="jsonschema", data=SCHEMA_TF2_MSGS,
        )
        ch_near_miss_encoding = w.register_channel(
            topic="/near_miss_encoding", message_encoding="json", schema_id=schema_tf2_json,
        )

        # 5. Ordinary channel, unaffected by the whitelist.
        schema_float = w.register_schema(
            name="std_msgs/Float64", encoding="ros2msg", data=SCHEMA_FLOAT64,
        )
        ch_ordinary = w.register_channel(
            topic="/sensor/value2", message_encoding="cdr", schema_id=schema_float,
        )

        for i in range(n_msgs):
            ts = i * dt_ns
            w.add_message(channel_id=ch_tf, log_time=ts, publish_time=ts, data=cdr_float64(1.0))
            # Dummy protobuf bytes -- doesn't need to be valid, the dialog never
            # decodes message content, only reads channel/schema metadata.
            w.add_message(channel_id=ch_foxglove, log_time=ts, publish_time=ts, data=b"\x00" * 8)
            w.add_message(
                channel_id=ch_near_miss_encoding, log_time=ts, publish_time=ts, data=b'{"x":1}',
            )
            w.add_message(channel_id=ch_ordinary, log_time=ts, publish_time=ts, data=cdr_float64(2.0))

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

if __name__ == "__main__":
    print("Generating verification MCAPs...\n")

    gen_publish_vs_log_time()
    gen_embedded_timestamp()
    gen_dialog_whitelist()
    gen_large_arrays()
    gen_empty_channel()
    gen_unsupported_encoding()
    gen_bad_schema()
    gen_large_file()

    print("\nDone. Each file's generator docstring above describes what it verifies.")
