# Foxglove Bridge Streaming Source

Connects to a [Foxglove WebSocket](https://docs.foxglove.dev/docs/connecting-to-data/frameworks/custom#foxglove-websocket)
server and streams real-time data via delegated ingest.

## Features

- WebSocket connection with configurable address and port
- Automatic channel discovery via Foxglove `advertise` protocol
- Topic filtering by name in the dialog
- Per-channel parser delegation by encoding:
  - `ros2msg` / `omgidl` over CDR → `parser_ros`
  - `protobuf` (binary `FileDescriptorSet` schema, base64-encoded in the advertise) → `parser_protobuf`
  - `json` (JSON-Schema text or schemaless) → `parser_json`, with
    `label_keyed_arrays` enabled so `{"scalars":[{"label":"shoulder_pan.pos","value":…}]}`
    payloads (e.g. LeRobot 0.6.0's `--display_mode=foxglove` joint/action
    channels) plot as one series per label
- Configurable array size limits and embedded timestamp usage

> **Note:** protobuf and json routing are intentional, additive enhancements
> beyond the upstream PlotJuggler plugin, which handled only CDR/ros2msg. The
> Foxglove WebSocket protocol is multi-encoding, so each channel is classified
> and routed to the matching parser (`classifyChannel` in
> `foxglove_protocol.hpp`). Encodings with no parser yet (`flatbuffer`) are
> reported as a visible warning instead of being silently dropped.

## Configuration

The dialog connects to a Foxglove Bridge server, discovers available
channels, and lets you select which topics to subscribe to. Parser options
(array size, clamp/skip, embedded timestamp) are configurable.

## Known Limitations

- No pause/resume support (requires host-side streamer controls)
- Unadvertise messages from server not handled
- No reconnection on unexpected disconnect
- Server status/warning messages not processed
