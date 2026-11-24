# Changelog — data_stream_foxglove_bridge

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_stream_foxglove_bridge/`.

## Upcoming

### Changed
- The dialog takes the server as one URL line (`ws://localhost:8765` by default) instead of address and port fields. A bare host or `host:port` gets `ws://` and port 8765; a query token is dialed as typed; an invalid URL disables Connect and the status line says why. A URL with credentials (`user:password@`) or a fragment (`#`) is refused: neither PlotJuggler nor browsers send credentials, and WebSocket URLs cannot have a fragment. The configuration saves `url`; configurations with `address`/`port` still load.
- The last connected URL is remembered, cleaned of secret query parameters, in PlotJuggler's settings store; it fills the line when no saved configuration names one (on first open, after a browser reload). The status line and connection messages show the URL cleaned the same way.
- Select All (Ctrl+A) and Deselect All (Ctrl+Shift+A) buttons; Select All follows the filter. The channel list is titled "Topics", like the other streamers.
- Transform channels (`tf2_msgs/msg/TFMessage` over CDR, `foxglove.FrameTransform` over protobuf) are always subscribed: their rows are selected, locked and tooltipped, Deselect All, a filter or a saved selection never drops them, and they are saved with the selection, so a PlotJuggler without demand subscriptions subscribes them too.

### Fixed
- On a PlotJuggler without demand subscriptions, a channel the server advertises again is not subscribed a second time.

## [1.3.1] - 2026-11-19

### Changed
- The dialog's footer and buttons are drawn by PlotJuggler's dialog host, and the plugin's own button margins are removed. Settings and behaviour are unchanged.

## [1.3.0] - 2026-11-15

### Added
- Ready for the browser build of PlotJuggler: the plugin's WebSocket transport (shared with the PlotJuggler Bridge) has a backend on the browser's own WebSocket.
- A full `ws://` or `wss://` URL (path included, e.g. `wss://robot.local:8765`) can be typed into the address field, or saved as the `url` setting; it replaces address and port. A page served over `https://` can only open `wss://`, so the browser build needs TLS in front of `foxglove_bridge`.
- The connection dialog retries a refused, timed-out or dropped connection with a capped backoff until cancelled, and shows connecting / retrying in N s with the last error / connected, as the PlotJuggler Bridge dialog does.

### Changed
- Starting the stream no longer blocks while connecting. A first connection that does not open within 5 s, or is refused, stops the stream with an error; a connection lost later is still retried every 5 s.
- Unprocessed data is bounded (a 512 MiB queue on the desktop, 64 MiB in the browser; with the batch being applied, up to about twice that): when PlotJuggler falls behind, the oldest messages are dropped and a warning reports how many, so the plots have gaps instead of memory growing without limit.

### Fixed
- Connects to `foxglove_bridge` releases that predate the Foxglove SDK: the handshake now offers the older `foxglove.websocket.v1` subprotocol next to `foxglove.sdk.v1`, and the server picks the one it speaks. ROS 1 channels are still not decoded.
- Messages, advertisements and removals are applied in the order the server sent them; before, a message that arrived just before its channel's `unadvertise` could be dropped.
- A malformed control message from the server is dropped with a warning instead of escaping the poll; an advertised channel without a valid id is ignored, and the valid ids of a malformed removal are still applied.
- Taking over the dialog's connection on OK no longer races with the socket's own thread.
- On a PlotJuggler without on-demand topics, starting from a saved selection subscribes the selected topics by name from the server's advertisement, once each, instead of also subscribing the channel ids saved from an earlier session (wrong data if the server renumbered its channels, a duplicate subscription otherwise).
- A channel the server removes while the dialog is open is no longer handed to the stream.
- After reconnecting to a server that advertises no channels, PlotJuggler no longer lists the previous session's topics.
- The transport indicator follows a URL edited from `ws://` to `wss://` (or back).
- A saved port outside 0–65535 is rejected instead of being silently wrapped (70000 became 4464).
- A connection that opens only after the 5 s deadline counts as timed out, even when it opened before PlotJuggler next checked; the dialog retries it.
- Browser build: stopping the stream unsubscribes on the server before closing the socket, and a send the browser cannot queue drops the connection (and reconnects) instead of losing the frame silently.

## [1.1.2] - 2026-11-09

### Changed
- Updated the bundled IXWebSocket from 11.4.6 to 12.0.0, picking up upstream bug and security fixes.

## [1.1.1] - 2026-11-05

### Changed
- Maintenance release with no intended user-facing behavior changes.

## [1.1.0] - 2026-08-04
