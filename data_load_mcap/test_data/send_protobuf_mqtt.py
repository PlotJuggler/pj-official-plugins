#!/usr/bin/env python3
"""
Send SensorReading protobuf messages via MQTT to localhost:1883.
Topic: pj4/sensor/reading
"""
import math
import time
import paho.mqtt.client as mqtt
from google.protobuf import descriptor_pb2, descriptor_pool, message_factory

HOST = "localhost"
PORT = 1883
TOPIC = "pj4/sensor/reading"


def make_sensor_reading_class():
    fdp = descriptor_pb2.FileDescriptorProto()
    fdp.name = "test_sensor_mqtt.proto"
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
        f.name = name; f.number = number
        f.type = ftype
        f.label = FieldDescriptorProto.LABEL_OPTIONAL
    pool = descriptor_pool.DescriptorPool()
    pool.Add(fdp)
    factory = message_factory.MessageFactory(pool=pool)
    desc = pool.FindMessageTypeByName("SensorReading")
    return factory.GetPrototype(desc)


def main():
    SensorReading = make_sensor_reading_class()
    client = mqtt.Client()
    client.connect(HOST, PORT)
    client.loop_start()

    print(f"Publishing SensorReading to {HOST}:{PORT} topic={TOPIC}")
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
        client.publish(TOPIC, msg.SerializeToString())
        print(f"\r  t={t:.1f}s  temp={msg.temperature:.2f}°C", end="")
        t += 0.1; i += 1
        time.sleep(0.1)


if __name__ == "__main__":
    main()
