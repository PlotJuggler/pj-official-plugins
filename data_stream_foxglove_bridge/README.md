# Foxglove Bridge Streaming Source

Connects to a [Foxglove WebSocket](https://docs.foxglove.dev/docs/connecting-to-data/frameworks/custom#foxglove-websocket)
server and streams real-time data via delegated ingest.

## Features

- WebSocket connection with configurable address and port
- Automatic channel discovery via Foxglove `advertise` protocol
- Topic filtering by name in the dialog
- Parser delegation based on channel encoding (typically `ros2msg` / CDR)
- Configurable array size limits and embedded timestamp usage

## Configuration

The dialog connects to a Foxglove Bridge server, discovers available
channels, and lets you select which topics to subscribe to. Parser options
(array size, clamp/skip, embedded timestamp) are configurable.

## Known Limitations

- No pause/resume support (requires host-side streamer controls)
- Unadvertise messages from server not handled
- No reconnection on unexpected disconnect
- Server status/warning messages not processed
