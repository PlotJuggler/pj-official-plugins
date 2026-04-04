#!/usr/bin/env python3
"""
Unified MQTT test publisher for PlotJuggler.

Supports multiple encoding modes:
  - json: JSON without embedded timestamp
  - json_ts: JSON with embedded timestamp field
  - protobuf: Protobuf without timestamp field populated
  - protobuf_ts: Protobuf with timestamp field populated

Usage:
  python mqtt_publisher.py --mode json
  python mqtt_publisher.py --mode json_ts
  python mqtt_publisher.py --mode protobuf
  python mqtt_publisher.py --mode protobuf_ts
  python mqtt_publisher.py --mode protobuf --host 192.168.1.100 --port 1883

Requirements:
  pip install paho-mqtt protobuf

To generate protobuf bindings:
  protoc --python_out=. test_message.proto
"""

import argparse
import json
import math
import sys
import time

try:
    import paho.mqtt.client as mqtt
    from paho.mqtt.enums import CallbackAPIVersion
    PAHO_V2 = True
except ImportError:
    try:
        import paho.mqtt.client as mqtt
        PAHO_V2 = False
    except ImportError:
        print("Error: paho-mqtt not installed. Run: pip install paho-mqtt")
        sys.exit(1)

# Try to import protobuf bindings
try:
    import test_message_pb2
    PROTOBUF_AVAILABLE = True
except ImportError:
    PROTOBUF_AVAILABLE = False


def on_connect(client, userdata, flags, *args):
    """Callback when connected to broker."""
    # Handle both paho v1 and v2 API
    rc = args[0] if len(args) == 1 else args[1] if len(args) >= 2 else 0
    print(f"Connected to MQTT broker (rc={rc})")


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
        description="MQTT test publisher for PlotJuggler",
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
        "--host", "-H",
        default="127.0.0.1",
        help="MQTT broker host (default: 127.0.0.1)"
    )
    parser.add_argument(
        "--port", "-p",
        type=int,
        default=1883,
        help="MQTT broker port (default: 1883)"
    )
    parser.add_argument(
        "--topic", "-t",
        default="plotjuggler/test",
        help="MQTT topic (default: plotjuggler/test)"
    )
    parser.add_argument(
        "--rate", "-r",
        type=float,
        default=10.0,
        help="Publish rate in Hz (default: 10)"
    )
    parser.add_argument(
        "--qos", "-q",
        type=int,
        choices=[0, 1, 2],
        default=0,
        help="MQTT QoS level (default: 0)"
    )
    args = parser.parse_args()

    # Check protobuf availability
    if args.mode.startswith("protobuf") and not PROTOBUF_AVAILABLE:
        print("Error: Protobuf mode requires test_message_pb2.py")
        print("Generate it with: protoc --python_out=. test_message.proto")
        sys.exit(1)

    # Create MQTT client
    if PAHO_V2:
        client = mqtt.Client(
            callback_api_version=CallbackAPIVersion.VERSION2,
            client_id="pj-test-publisher"
        )
    else:
        client = mqtt.Client(client_id="pj-test-publisher")

    client.on_connect = on_connect

    # Connect
    print(f"Connecting to {args.host}:{args.port}...")
    try:
        client.connect(args.host, args.port, 60)
    except Exception as e:
        print(f"Error connecting to broker: {e}")
        sys.exit(1)

    client.loop_start()

    # Determine payload creator
    include_ts = args.mode.endswith("_ts")
    if args.mode.startswith("json"):
        create_payload = lambda t: create_json_payload(t, include_ts)
        encoding_name = "JSON"
    else:
        create_payload = lambda t: create_protobuf_payload(t, include_ts)
        encoding_name = "Protobuf"

    ts_status = "WITH" if include_ts else "WITHOUT"
    print(f"Publishing {encoding_name} {ts_status} timestamp to '{args.topic}' at {args.rate} Hz")
    print("Press Ctrl+C to stop")

    t = 0.0
    dt = 1.0 / args.rate

    try:
        while True:
            payload = create_payload(t)
            result = client.publish(args.topic, payload, qos=args.qos)

            if t == 0.0 or int(t) % 5 == 0 and abs(t - int(t)) < dt:
                print(f"t={t:.1f}s, payload_size={len(payload)} bytes")

            t += dt
            time.sleep(dt)

    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
