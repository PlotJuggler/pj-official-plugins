# ZeroMQ Streaming Source

Receives data via a ZeroMQ SUB socket and hands it to message parsers.

## Configuration

The dialog configures transport (tcp, ipc, pgm), address, port (unused for ipc),
connect or bind mode, and the topic filter: a comma, semicolon or space separated
list of SUB subscriptions (empty subscribes to everything).

## Message frames

- `[payload]`: no topic (the topic is then `zmq/data`)
- `[topic, payload]`
- `[topic, payload, timestamp]`

The optional third frame is a text-encoded timestamp (seconds since epoch). When it
is absent or not a number, the arrival time is used.

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

The `test_message.proto` schema is used for protobuf modes.
