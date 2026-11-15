# Changelog — data_stream_foxglove_bridge

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_stream_foxglove_bridge/`.

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
