# MQTT Streaming Source

Connects to an MQTT broker and streams messages via delegated ingest
to message parsers.

## Features

- MQTT connection with configurable address, port, and QoS level
- Topic filter subscription (e.g. `#` for all topics, or specific patterns)
- Live topic discovery via a dedicated async client during dialog
- Per-topic parser binding and caching
- Username/password authentication
- SSL/TLS with configurable CA certificate, client certificate, and private key
- MQTT protocol version selection (3.1, 3.1.1, 5.0)
- Connection-loss detection and warning

## Configuration

The dialog configures broker address, port, topic filter, QoS level,
SSL toggle and certificate paths, username/password, and MQTT protocol version.
Parser encoding is selected per-topic in the dialog.
