#!/usr/bin/env python3
"""
Send SensorReading protobuf messages via UDP to localhost:9870.
Use with the UDP stream plugin in PJ4:
  - Select encoding: protobuf
  - In ProtobufParserDialog: load test_sensor.proto, select SensorReading
  - Enable "Use embedded timestamp field", leave field name empty
  - The 'timestamp' field starts at t=0 s while the receive time starts later
"""

import math
import socket
import time

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory

HOST = "127.0.0.1"
PORT = 9870


def make_sensor_reading_class():
    fdp = descriptor_pb2.FileDescriptorProto()
    fdp.name = "test_sensor_udp.proto"
    fdp.syntax = "proto3"
    msg = fdp.message_type.add()
    msg.name = "SensorReading"
    from google.protobuf.descriptor_pb2 import FieldDescriptorProto
    for name, number, ftype in [
        ("timestamp",   1, FieldDescriptorProto.TYPE_DOUBLE),
        ("temperature", 2, FieldDescriptorProto.TYPE_DOUBLE),
        ("pressure",    3, FieldDescriptorProto.TYPE_DOUBLE),
        ("humidity",    4, FieldDescriptorProto.TYPE_DOUBLE),
    ]:
        f = msg.field.add()
        f.name = name
        f.number = number
        f.type = ftype
        f.label = FieldDescriptorProto.LABEL_OPTIONAL

    pool = descriptor_pool.DescriptorPool()
    pool.Add(fdp)
    factory = message_factory.MessageFactory(pool=pool)
    desc = pool.FindMessageTypeByName("SensorReading")
    return factory.GetPrototype(desc)


def main():
    SensorReading = make_sensor_reading_class()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    print(f"Sending SensorReading to {HOST}:{PORT} every 100ms")
    print("  'timestamp' field starts at t=0 s (sensor time)")
    print("  receive time starts whenever you open the stream")
    print("  Ctrl+C to stop")

    t = 0.0
    i = 0
    while True:
        msg = SensorReading(
            timestamp=round(t, 4),
            temperature=round(20.0 + math.log1p(i) * 0.2, 4),
            pressure=round(1014.0 - i * 0.018 + math.sin(i * 0.3) * 0.05, 4),
            humidity=round(50.0 + math.sin(i * 0.1) * 5.0, 4),
        )
        data = msg.SerializeToString()
        sock.sendto(data, (HOST, PORT))
        print(f"\r  t={t:.1f}s  temp={msg.temperature:.2f}°C  "
              f"press={msg.pressure:.2f}hPa  hum={msg.humidity:.2f}%", end="")
        t += 0.1
        i += 1
        time.sleep(0.1)


if __name__ == "__main__":
    main()
