# Changelog — data_stream_pj_bridge

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_stream_pj_bridge/`.

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
