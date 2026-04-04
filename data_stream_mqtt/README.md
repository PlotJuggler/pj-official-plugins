# MQTT Streaming Source

Connects to an MQTT broker and streams messages via delegated ingest
to message parsers.

## Features

- MQTT connection with configurable address, port, and QoS level
- Topic filter subscription (e.g. `#` for all topics, or specific patterns)
- Message queuing with poll-based delivery
- Per-topic parser binding and caching

## Configuration

The dialog configures broker address, port, topic filter, QoS level,
and SSL toggle. Parser encoding is auto-detected from message content.

## Testing

Test publisher scripts are in `test_scripts/`. Requires `pip install paho-mqtt protobuf`.

```bash
cd test_scripts/

# JSON without embedded timestamp
./mqtt_publisher.py --mode json

# JSON with embedded timestamp field
./mqtt_publisher.py --mode json_ts

# Protobuf without timestamp
./mqtt_publisher.py --mode protobuf

# Protobuf with timestamp field
./mqtt_publisher.py --mode protobuf_ts

# Custom broker/topic/rate
./mqtt_publisher.py --mode json_ts --host 192.168.1.100 --topic sensors --rate 20
```

Options: `--host`, `--port`, `--topic`, `--rate`, `--qos`.

The `test_message.proto` schema is used for protobuf modes.

## Known Limitations

- TLS certificate management not yet available (only on/off toggle)
- Live topic discovery not implemented (manual filter only)
- Username/password authentication not exposed in dialog
- No reconnection detection or retry logic
- MQTT protocol version not configurable (uses library default)
