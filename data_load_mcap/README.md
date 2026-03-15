# MCAP Data Loader

Imports [MCAP](https://mcap.dev/) files containing timestamped messages with
schema-based decoding via delegated ingest to message parsers.

## Features

- Selective summary reading for fast channel/schema discovery
- Topic filtering by name (space-separated AND matching)
- Configurable array size limits and clamping
- Choice of publish time vs. log time for timestamps
- Delegated ingest: message decoding handled by parser plugins (ROS, Protobuf, JSON, etc.)

## Configuration

The dialog shows all channels with schema and encoding info. Select topics
to import. Options include max array size, clamp vs. discard policy, and
embedded timestamp usage.

## Known Limitations

- Keyboard shortcuts (Ctrl+A/Ctrl+Shift+A) for select/deselect not yet available
- Channels with zero messages are selectable (should be grayed out)
