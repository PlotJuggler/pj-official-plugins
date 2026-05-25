#!/usr/bin/env python3
"""
Generate test_protobuf_embedded_ts.mcap:
  - 50 messages of SensorReading (protobuf)
  - embedded 'timestamp' field starts at t=0 s
  - MCAP log_time starts at t=5 s
  - If embedded timestamp is used: series start at t=0 s
  - If disabled: series start at t=5 s

How to open in PJ4:
  1. File -> Open -> test_protobuf_embedded_ts.mcap
  2. In McapDialog: comboBoxProtocol selects "protobuf"
  3. In ProtobufParserDialog (pj_parser_slot):
     - Click "Load .proto file" -> select test_sensor.proto
     - Select message type: SensorReading
     - Enable "Use embedded timestamp field", leave field name empty
  4. Accept
  Expected: series start at t=0 s (not t=5 s)
"""

import math
import struct
from pathlib import Path

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
from mcap.writer import Writer

OUT = Path(__file__).parent


def build_file_descriptor_set():
    """Build a FileDescriptorSet for SensorReading."""
    fdp = descriptor_pb2.FileDescriptorProto()
    fdp.name = "test_sensor.proto"
    fdp.syntax = "proto3"

    msg = fdp.message_type.add()
    msg.name = "SensorReading"

    def add_field(msg, name, number, type_):
        f = msg.field.add()
        f.name = name
        f.number = number
        f.type = type_
        f.label = descriptor_pb2.FieldDescriptorProto.LABEL_OPTIONAL

    from google.protobuf.descriptor_pb2 import FieldDescriptorProto
    add_field(msg, "timestamp",   1, FieldDescriptorProto.TYPE_DOUBLE)
    add_field(msg, "temperature", 2, FieldDescriptorProto.TYPE_DOUBLE)
    add_field(msg, "pressure",    3, FieldDescriptorProto.TYPE_DOUBLE)
    add_field(msg, "humidity",    4, FieldDescriptorProto.TYPE_DOUBLE)

    fds = descriptor_pb2.FileDescriptorSet()
    fds.file.append(fdp)
    return fds


def make_message_class(fds):
    pool = descriptor_pool.DescriptorPool()
    for f in fds.file:
        pool.Add(f)
    factory = message_factory.MessageFactory(pool=pool)
    desc = pool.FindMessageTypeByName("SensorReading")
    return factory.GetPrototype(desc)


def gen_protobuf_embedded_ts():
    fds = build_file_descriptor_set()
    SensorReading = make_message_class(fds)

    path = OUT / "test_protobuf_embedded_ts.mcap"
    n_msgs       = 50
    dt_ns        = 100_000_000   # 100 ms
    log_start_ns = 5_000_000_000 # log_time starts at t=5 s
    ts_start_s   = 0.0           # embedded timestamp starts at t=0 s

    with open(path, "wb") as f:
        w = Writer(f)
        w.start(profile="", library="pj-verification")

        schema_id = w.register_schema(
            name="SensorReading",
            encoding="protobuf",
            data=fds.SerializeToString(),
        )
        ch = w.register_channel(
            topic="/sensor/reading",
            message_encoding="protobuf",
            schema_id=schema_id,
        )

        for i in range(n_msgs):
            log_ts = log_start_ns + i * dt_ns
            t = ts_start_s + i * 0.1

            msg = SensorReading(
                timestamp=round(t, 4),
                temperature=round(20.0 + math.log1p(i) * 0.2, 4),
                pressure=round(1014.0 - i * 0.018 + math.sin(i * 0.3) * 0.05, 4),
                humidity=round(50.0 + math.sin(i * 0.1) * 5.0, 4),
            )
            w.add_message(
                channel_id=ch,
                log_time=log_ts,
                publish_time=log_ts,
                data=msg.SerializeToString(),
            )

        w.finish()

    print(f"[OK] {path.name:45s} {path.stat().st_size:>8} bytes")
    print(f"     proto: {(OUT / 'test_sensor.proto').resolve()}")


if __name__ == "__main__":
    gen_protobuf_embedded_ts()
