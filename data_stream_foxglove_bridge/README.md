# Foxglove Bridge Streaming Source

Connects to a [Foxglove WebSocket](https://docs.foxglove.dev/docs/connecting-to-data/frameworks/custom#foxglove-websocket)
server and streams real-time data via delegated ingest.

## Features

- WebSocket connection to a server URL typed in one line (see [Configuration](#configuration))
- The dialog retries a refused or dropped connection with a capped backoff until cancelled; a stream whose connection is lost reconnects every 5 s
- Connects to current servers (`foxglove.sdk.v1` subprotocol) and older ones (`foxglove.websocket.v1`); nothing to configure
- Automatic channel discovery via Foxglove `advertise` protocol
- Topic filtering by name and type in the dialog, Select All / Deselect All
- Transform channels locked in the dialog and always advertised (see [Configuration](#configuration))
- Per-channel parser delegation by encoding:
  - `ros2msg` / `omgidl` over CDR → `parser_ros`
  - `protobuf` (binary `FileDescriptorSet` schema, base64-encoded in the advertise) → `parser_protobuf`
  - `json` (JSON-Schema text or schemaless) → `parser_json`, with
    `label_keyed_arrays` enabled so `{"scalars":[{"label":"shoulder_pan.pos","value":…}]}`
    payloads (e.g. LeRobot 0.6.0's `--display_mode=foxglove` joint/action
    channels) plot as one series per label
- Configurable array size limits and embedded timestamp usage

Each channel is classified and routed to its parser by `classifyChannel` in
`foxglove_protocol.hpp`. Encodings with no parser (`flatbuffer`) are reported
as a warning instead of being silently dropped.

## Configuration

The dialog connects to a Foxglove Bridge server, discovers available
channels, and lets you select which topics to subscribe to. Parser options
(array size, clamp/skip, embedded timestamp) are configurable.

| Key | Meaning |
|---|---|
| `url` | The server URL (e.g. `wss://robot.local:8765`), saved normalized. Loading accepts the same forgiving input as the dialog; an invalid one is rejected. |
| `address`, `port` | Legacy keys, read only when `url` is absent: `ws://<address>:<port>` (default `127.0.0.1:8765`). Never written. |

The dialog takes the server as one URL line (`ws://127.0.0.1:8765` by default); a bare host or
`host:port` gets `ws://` and port 8765. Which URLs are accepted or refused (credentials and
fragments are), how the URL is saved, and how the last connected URL is remembered and cleaned
of secrets: [The server URL](../common/websocket/README.md#the-server-url).

**Select All** (Ctrl+A) selects the topics the filter shows; **Deselect All** (Ctrl+Shift+A)
clears every selection, also those the filter hides.

Transform channels are every advertised channel whose (type, message encoding) is
`tf2_msgs/msg/TFMessage` over `cdr` or `foxglove.FrameTransform` over `protobuf`. Their rows
are locked, and the selection never filters them out; the rule, shared with the MCAP loader and
the other streamers, is in
[`always_included.hpp`](../common/always_included/include/pj_always_included/always_included.hpp).

To try it, replay the MCAP loader's transform fixture, which advertises `/tf`, `/tf_static`,
a `foxglove.FrameTransform` channel, a JSON look-alike that is not locked and an ordinary
channel: `python3 test_scripts/foxglove_mcap_player.py ../data_load_mcap/test_data/test_dialog_whitelist.mcap --loop`.

## Known Limitations

- No pause/resume support (requires host-side streamer controls)
- ROS 1 channels (`ros1` encoding, `ros1msg` schemas) are not decoded: a ROS 1 bridge connects and lists its topics, but they produce no data

## Threading and transport

The transport is the shared [`common/websocket`](../common/websocket/README.md)
client, also used by the PlotJuggler Bridge, so the plugin builds for the desktop
(IXWebSocket) and for PlotJuggler's browser build (`<emscripten/websocket.h>`) alike;
the dialog's connection fields, retry and status line come from
`<pj_websocket/connection_dialog.hpp>`.

- Every transport event (open, text and binary frames, close, error) is copied into one
  bounded FIFO and applied in arrival order on the poll thread (on the dialog's tick while
  the dialog owns the connection), so no transport thread ever touches a parser binding.
  A connection adopted from the dialog is opened, and the selection subscribed, on the
  first poll.
- The queue holds at most 512 MiB of unprocessed binary frames on the desktop and 64 MiB
  in the browser. When the poll thread falls behind, the OLDEST are dropped and a warning
  reports the running count, at most every 5 s; text frames (advertise, unadvertise,
  status) are never dropped. The poll thread applies one drained batch while the queue
  refills, so unprocessed data can peak at about twice the cap (1 GiB / 128 MiB).
- A page served over `https://` can open only `wss://` sockets (localhost excepted), so a
  plain `ws://` `foxglove_bridge` on a robot needs TLS in front of it for the browser build.
