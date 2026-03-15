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

## Known Limitations

- TLS certificate management not yet available (only on/off toggle)
- Live topic discovery not implemented (manual filter only)
- Username/password authentication not exposed in dialog
- No reconnection detection or retry logic
- MQTT protocol version not configurable (uses library default)
