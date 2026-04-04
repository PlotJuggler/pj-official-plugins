#!/usr/bin/env python3
"""
Unified ZMQ test publisher for PlotJuggler.

Supports multiple encoding modes:
  - json: JSON without embedded timestamp
  - json_ts: JSON with embedded timestamp field
  - protobuf: Protobuf without timestamp field populated
  - protobuf_ts: Protobuf with timestamp field populated

ZMQ message format (multipart):
  - Frame 0: topic (optional, if --topic is set)
  - Frame 1: payload (JSON or Protobuf bytes)
  - Frame 2: timestamp as string (optional, if --zmq-timestamp is set)

Usage:
  python zmq_publisher.py --mode json
  python zmq_publisher.py --mode json_ts --topic sensors
  python zmq_publisher.py --mode protobuf_ts --topic robot --zmq-timestamp
  python zmq_publisher.py --mode protobuf --port 9999

Requirements:
  pip install pyzmq protobuf

To generate protobuf bindings:
  protoc --python_out=. test_message.proto
"""

import argparse
import json
import math
import sys
import time

try:
    import zmq
except ImportError:
    print("Error: pyzmq not installed. Run: pip install pyzmq")
    sys.exit(1)

# Try to import protobuf bindings
try:
    import test_message_pb2
    PROTOBUF_AVAILABLE = True
except ImportError:
    PROTOBUF_AVAILABLE = False


def create_json_payload(t: float, include_timestamp: bool) -> bytes:
    """Create JSON payload with sensor data."""
    data = {
        "sensors": {
            "temperature": 20.0 + 5.0 * math.sin(t * 0.5),
            "pressure": 1013.25 + 10.0 * math.cos(t * 0.3),
            "humidity": 50.0 + 20.0 * math.sin(t * 0.2),
        },
        "value_sin": math.sin(t),
        "value_cos": math.cos(t),
        "counter": int(t * 10),
    }
    if include_timestamp:
        data["timestamp"] = t
    return json.dumps(data).encode()


def create_protobuf_payload(t: float, include_timestamp: bool) -> bytes:
    """Create Protobuf payload with sensor data."""
    if not PROTOBUF_AVAILABLE:
        raise RuntimeError("Protobuf bindings not available")

    msg = test_message_pb2.TestMessage()
    if include_timestamp:
        msg.timestamp = t
    msg.value_sin = math.sin(t)
    msg.value_cos = math.cos(t)
    msg.counter = int(t * 10)
    msg.sensors.temperature = 20.0 + 5.0 * math.sin(t * 0.5)
    msg.sensors.pressure = 1013.25 + 10.0 * math.cos(t * 0.3)
    msg.sensors.humidity = 50.0 + 20.0 * math.sin(t * 0.2)
    return msg.SerializeToString()


def main():
    parser = argparse.ArgumentParser(
        description="ZMQ test publisher for PlotJuggler",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument(
        "--mode", "-m",
        choices=["json", "json_ts", "protobuf", "protobuf_ts"],
        default="json",
        help="Encoding mode (default: json)"
    )
    parser.add_argument(
        "--port", "-p",
        type=int,
        default=9872,
        help="ZMQ port to bind (default: 9872)"
    )
    parser.add_argument(
        "--topic", "-t",
        default=None,
        help="ZMQ topic prefix (optional, sent as first frame)"
    )
    parser.add_argument(
        "--zmq-timestamp",
        action="store_true",
        help="Send timestamp as third ZMQ frame (ZMQ-level timestamp)"
    )
    parser.add_argument(
        "--rate", "-r",
        type=float,
        default=10.0,
        help="Publish rate in Hz (default: 10)"
    )
    args = parser.parse_args()

    # Check protobuf availability
    if args.mode.startswith("protobuf") and not PROTOBUF_AVAILABLE:
        print("Error: Protobuf mode requires test_message_pb2.py")
        print("Generate it with: protoc --python_out=. test_message.proto")
        sys.exit(1)

    # Create ZMQ socket
    context = zmq.Context()
    socket = context.socket(zmq.PUB)
    bind_addr = f"tcp://*:{args.port}"
    socket.bind(bind_addr)
    print(f"ZMQ PUB socket bound to {bind_addr}")

    # Determine payload creator
    include_ts = args.mode.endswith("_ts")
    if args.mode.startswith("json"):
        create_payload = lambda t: create_json_payload(t, include_ts)
        encoding_name = "JSON"
    else:
        create_payload = lambda t: create_protobuf_payload(t, include_ts)
        encoding_name = "Protobuf"

    ts_status = "WITH" if include_ts else "WITHOUT"
    zmq_ts_status = "+ ZMQ timestamp frame" if args.zmq_timestamp else ""
    topic_status = f"topic='{args.topic}'" if args.topic else "no topic"

    print(f"Publishing {encoding_name} {ts_status} embedded timestamp, {topic_status} {zmq_ts_status}")
    print(f"Rate: {args.rate} Hz")
    print("Press Ctrl+C to stop")
    print()

    t = 0.0
    dt = 1.0 / args.rate

    try:
        while True:
            payload = create_payload(t)

            # Build multipart message
            frames = []
            frame_desc = []

            if args.topic:
                frames.append(args.topic.encode())
                frame_desc.append(f"topic={args.topic}")

            frames.append(payload)
            frame_desc.append(f"payload={len(payload)}b")

            if args.zmq_timestamp:
                ts_ns = time.time_ns()
                ts_str = str(ts_ns * 1e-9)
                frames.append(ts_str.encode())
                frame_desc.append(f"ts={ts_str[:10]}...")

            socket.send_multipart(frames)

            if t == 0.0 or int(t) % 5 == 0 and abs(t - int(t)) < dt:
                print(f"t={t:.1f}s [{', '.join(frame_desc)}]")

            t += dt
            time.sleep(dt)

    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        socket.close()
        context.term()


if __name__ == "__main__":
    main()
