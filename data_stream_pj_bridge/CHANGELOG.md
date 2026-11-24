# Changelog — data_stream_pj_bridge

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_stream_pj_bridge/`.

## Upcoming

### Changed
- The dialog takes the server as one URL line (`ws://127.0.0.1:9090` by default) instead of address, port and transport fields. A bare host or `host:port` gets `ws://` and port 9090; a query token is dialed as typed; an invalid URL disables Connect and the status line says why. A URL with credentials (`user:password@`) or a fragment (`#`) is refused: neither PlotJuggler nor browsers send credentials, and WebSocket URLs cannot have a fragment. The configuration saves `url`; configurations with `address`/`port` still load.
- The last connected URL is remembered, cleaned of secret query parameters, in PlotJuggler's settings store; it fills the line when no saved configuration names one (on first open, after a browser reload). The status line and connection messages show the URL cleaned the same way.
- Select All (Ctrl+A) and Deselect All (Ctrl+Shift+A) buttons; Select All follows the filter.
- `tf2_msgs/msg/TFMessage` topics are always subscribed: their rows are selected, locked and tooltipped, Deselect All, a filter or a saved selection never drops them, and they are saved with the selection, so a PlotJuggler without demand subscriptions subscribes them too.

## [1.1.2] - 2026-11-19

### Changed
- The dialog's footer and buttons are drawn by PlotJuggler's dialog host, and the plugin's own button margins are removed. Settings and behaviour are unchanged.

## [1.1.1] - 2026-11-15

### Changed
- Updated the bundled IXWebSocket from 11.4.6 to 12.0.0, picking up upstream bug and security fixes.
- Pause and resume are sent to the bridge on the stream's next poll instead of from the calling thread.
- A server that speaks a newer protocol closes the connection at once (the stream still stops with an upgrade message).

### Fixed
- Pausing or resuming while the stream is running no longer races the stream's own thread.
- One malformed topic entry from the bridge no longer empties the dialog's whole topic list.
- A protocol version too large for a 32-bit integer is recognised as newer and reported in full, instead of being wrapped to a supported one.
- The transport indicator follows a URL edited from `ws://` to `wss://` (or back).
- A saved port outside 0–65535 is rejected instead of being silently wrapped (70000 became 4464).
- A connection that opens only after the 5 s deadline counts as timed out, even when it opened before PlotJuggler next checked; the dialog retries it.
- Browser build: a send the browser cannot queue now stops the stream with an error instead of losing the frame silently.

## [1.1.0] - 2026-10-18

### Added
- A `url` setting (`ws://` or `wss://`, path included, e.g. `wss://robot.local:8443/bridge`)
  that replaces address and port. Type the full URL into the dialog's address field.
- Runs in the browser build of PlotJuggler: the plugin's WebSocket transport has a
  browser backend next to the desktop one.
- The connection dialog shows its state under the address: connecting, retrying in N s
  with the last error, or connected.

### Changed
- The default port is 9090, the port the PlotJuggler bridge server listens on (was 9871).
  Saved configurations keep the port they name.
- Starting the stream no longer waits for the connection: it connects, discovers topics
  and streams in the background, so a slow or unreachable bridge never freezes PlotJuggler.
  A failed connection (5 s timeout) or a dropped connection stops the stream with an
  error naming the bridge URL.
- The dialog keeps retrying a refused, unanswered or dropped connection, waiting 1 s, then
  2 s, 4 s and so on up to 10 s, until it connects or you press Cancel. Each attempt gives
  up after 5 s instead of after a minute.
- When PlotJuggler falls behind the stream (more than 512 MiB of unprocessed data on the
  desktop, 64 MiB in the browser), the oldest data frames are dropped and a warning reports
  how many so far; the stream keeps running.

### Fixed
- Starting from a saved configuration no longer crashes when the bridge is unreachable.

## [1.0.1] - 2026-08-04
