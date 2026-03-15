# PlotJuggler Bridge Streaming Source

Connects to a PlotJuggler WebSocket Bridge server for real-time data
streaming using the PJ binary protocol.

## Features

- WebSocket connection with configurable address and port
- Topic discovery with periodic refresh (~1s) while dialog is open
- Binary frame parsing: ZSTD-compressed multi-message payloads
- Delegated ingest with per-topic parser binding
- Parser options: array size, clamp/skip, embedded timestamp

## Protocol

The PJ Bridge protocol uses JSON for control messages (`get_topics`,
`subscribe`) and a custom binary format for data frames. Binary frames
have a 4-byte magic (`0x42524A50` = "PJRB"), message count, and
ZSTD-compressed payload containing topic/timestamp/CDR triples.

## Known Limitations

- No pause/resume commands (requires host-side streamer controls)
- Topic filtering in dialog not yet functional (TODO stub)
- No heartbeat keep-alive sent (risk of idle timeout)
- Wire format uses u32 for topic name length (original uses u16 — interop issue)
