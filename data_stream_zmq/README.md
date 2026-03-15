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

## Known Limitations

- Topic parsing handles whitespace differently from original (spaces
  preserved within topic names instead of stripped)
