#!/usr/bin/env python3
"""
MQTT publisher for RobotPose messages. robot_pose.proto imports common/types.proto,
so this exercises the Protobuf parser's include folders (steps in README_INCLUDE_FOLDERS.md).

Requirements: pip install paho-mqtt protobuf. The protos are compiled with protoc on first run.
"""

import sys
import time
import math

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("ERROR: paho-mqtt not installed. Run: pip install paho-mqtt")
    sys.exit(1)

# Compile the protos on first run.
try:
    import robot_pose_pb2
except ImportError:
    print("Proto modules not found. Compiling...")
    import subprocess
    import os

    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    result = subprocess.run([
        "protoc",
        "--python_out=.",
        "common/types.proto",
        "robot_pose.proto"
    ], capture_output=True, text=True)

    if result.returncode != 0:
        print(f"protoc failed: {result.stderr}")
        sys.exit(1)

    print("Protos compiled successfully")
    import robot_pose_pb2


def main():
    broker = "localhost"
    port = 1883
    topic = "robot/pose"

    print(f"Connecting to MQTT broker at {broker}:{port}")
    client = mqtt.Client()

    try:
        client.connect(broker, port, 60)
    except Exception as e:
        print(f"Failed to connect: {e}")
        print("Make sure mosquitto is running: mosquitto -p 1883")
        sys.exit(1)

    client.loop_start()

    print(f"Publishing RobotPose messages to topic '{topic}'")
    print("Press Ctrl+C to stop")
    print()
    print("To test in PlotJuggler:")
    print("  1. Open MQTT data source")
    print("  2. Select 'protobuf' encoding")
    print("  3. Load 'robot_pose.proto'")
    print("  4. Click 'Add folder...' and select 'test_scripts/' directory")
    print("  5. Select 'pj.test.RobotPose' message type")
    print("  6. Connect and observe the data")
    print()

    seq = 0
    t0 = time.time()

    try:
        while True:
            t = time.time() - t0

            msg = robot_pose_pb2.RobotPose()

            # header.timestamp is what use_embedded_timestamp reads
            msg.header.timestamp = t
            msg.header.frame_id = "world"
            msg.header.seq = seq

            radius = 2.0
            omega = 0.5  # rad/s
            msg.position.x = radius * math.cos(omega * t)
            msg.position.y = radius * math.sin(omega * t)
            msg.position.z = 0.5 * math.sin(0.2 * t)

            angle = omega * t
            msg.orientation.w = math.cos(angle / 2)
            msg.orientation.x = 0.0
            msg.orientation.y = 0.0
            msg.orientation.z = math.sin(angle / 2)

            msg.linear_velocity.x = -radius * omega * math.sin(omega * t)
            msg.linear_velocity.y = radius * omega * math.cos(omega * t)
            msg.linear_velocity.z = 0.1 * math.cos(0.2 * t)

            msg.angular_velocity.x = 0.0
            msg.angular_velocity.y = 0.0
            msg.angular_velocity.z = omega

            msg.robot_name = "test_robot"
            msg.is_moving = True
            msg.battery_level = 100.0 - (t % 100)  # decreasing

            payload = msg.SerializeToString()
            client.publish(topic, payload)

            print(f"\r[{seq}] t={t:.2f}s pos=({msg.position.x:.2f}, {msg.position.y:.2f}, {msg.position.z:.2f})", end="")

            seq += 1
            time.sleep(0.1)  # 10 Hz

    except KeyboardInterrupt:
        print("\n\nStopping...")
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
