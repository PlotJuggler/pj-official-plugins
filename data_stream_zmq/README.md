# ZeroMQ Streaming Source

Receives data via ZeroMQ SUB socket with delegated ingest to message
parsers.

## Features

- ZeroMQ SUB socket with configurable transport (tcp, ipc, pgm)
- Connect or Bind mode toggle
- Topic filter subscription (comma/semicolon separated list)
- Multi-part message handling: topic frame, payload frame, optional
  timestamp frame
- Non-blocking polling with per-topic parser caching

## Timestamp Handling

If a third ZMQ frame is present, it is parsed as a text-encoded
timestamp (seconds since epoch) and converted to nanoseconds. Otherwise,
the current system time is used.

## Configuration

The dialog configures transport protocol, address, port, connect/bind
mode, and topic filter string.

## Testing

Test publisher scripts are in `test_scripts/`. Requires `pip install pyzmq protobuf`.

```bash
cd test_scripts/

# JSON without embedded timestamp
./zmq_publisher.py --mode json

# JSON with embedded timestamp + topic
./zmq_publisher.py --mode json_ts --topic sensors

# Protobuf with ZMQ-level timestamp frame (third frame)
./zmq_publisher.py --mode protobuf --zmq-timestamp

# Protobuf with BOTH embedded timestamp AND ZMQ timestamp frame
./zmq_publisher.py --mode protobuf_ts --topic robot --zmq-timestamp

# Custom port/rate
./zmq_publisher.py --mode json --port 9999 --rate 20
```

Options: `--port`, `--topic`, `--zmq-timestamp`, `--rate`.

ZMQ frame structure:
- `[payload]` — no topic, no ZMQ timestamp
- `[topic, payload]` — with topic
- `[topic, payload, timestamp]` — with topic and ZMQ timestamp frame

The `test_message.proto` schema is used for protobuf modes.

## Known Limitations

- Topic parsing handles whitespace differently from original (spaces
  preserved within topic names instead of stripped)
