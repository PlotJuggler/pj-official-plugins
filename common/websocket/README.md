# common/websocket

WebSocket client transport shared by the websocket streaming plugins
(`data_stream_pj_bridge`, `data_stream_foxglove_bridge`).

- `<pj_websocket/websocket_client.hpp>`: `PJ::WebSocket::Client`, a non-blocking client with
  one contract on every backend. `start(url, sink, subprotocols)` returns at once; open, text
  and binary frames, close and errors arrive as owned `Event`s in an `EventQueue` that the
  owner drains from its own thread. The queue bounds pending data frames (512 MiB desktop,
  64 MiB browser) by dropping the oldest, never control events; `DropWarning` throttles the
  warning about it. A source holds up to one drained batch while the queue refills, so its
  inbound data peaks at about twice that cap (1 GiB desktop, 128 MiB browser).
- `<pj_websocket/connection_dialog.hpp>`: the connection half of a streamer's dialog.
  `Endpoint` (the one-line server URL, the `url` config key with the legacy
  `address`/`port`, the remembered last URL) and `DialogConnection` (connect/cancel,
  per-attempt deadline, doubling capped retry backoff, the URL line, Connect button and
  status line, handoff of the live connection to the source on OK). See
  [The server URL](#the-server-url).
- `<pj_websocket/url.hpp>`: `normalizeUrl` (forgiving input to the URL to dial, or why it is
  not one) and `cleanUrl` (the URL without its userinfo, query and fragment).
- `<pj_websocket/source_connection.hpp>`: the connection half of a streaming source.
  `SourceConnection` adopts the dialog's open connection or dials a fresh one, enforces the
  connect deadline, latches the first stream-stopping failure (`poll()` returns it until
  `stop()`) and, when `reconnect` is set, re-dials a lost connection on a fixed delay. Frames
  reach the owner through handlers in arrival order, on the poll thread. Its messages show
  the URL through `cleanUrl`, also where a transport error quotes it.

## The server URL

The PlotJuggler Bridge and Foxglove Bridge dialogs take the server as one URL line, and their
`url` config key holds the same URL.

- **Forgiving input.** Surrounding spaces are ignored. A bare host or `host:port` (path and
  query allowed) gets `ws://` and, without a port, the plugin's default port. A text starts
  with a scheme only when what precedes `://` has none of `/?#@`, so in
  `robot/x?next=ws://a` the `ws://` belongs to the query. A `ws://` or `wss://` URL without a
  port uses the scheme's own port. A query right after the host or port gets a `/` before it
  (`robot:9090?token=t` dials `ws://robot:9090/?token=t`): IXWebSocket reads the port up to
  the next `/`.
- **Refused**, with Connect disabled and the reason in the status line: another scheme,
  spaces, a missing host, an unbracketed IPv6 address, a port outside 1-65535, credentials
  (`user:password@`: IXWebSocket parses and drops them and browsers do not send them, so a
  server needing them would fail without saying why) and a fragment (`#`, which RFC 6455
  forbids in a WebSocket URI). A token in the query is accepted and dialed as typed.
- **Saved** under `url` normalized, its query included, so it reaches PlotJuggler's saved
  dialog configuration and layouts; a line that is not a URL is not saved. A loaded `url`
  loses its userinfo and fragment before the check, so a config that holds them still connects
  (IXWebSocket ignores both). The legacy `address` and `port` keys are read only when `url` is
  absent, composed into `ws://address:port` and checked by the same rules; they are never
  written.
- **Remembered.** Once a connection opens, the URL it dialed is stored, cleaned, in
  PlotJuggler's settings store (`pj.settings.v1`, key `kLastUrlSettingsKey`, which survives a
  browser reload). The remembered URL fills the line when no saved configuration names one,
  e.g. on first open and after a browser reload; on the desktop, PlotJuggler's saved dialog
  configuration takes precedence. A host without the store (older PlotJuggler) remembers
  nothing.
- **Cleaned** (`cleanUrl`), for the remembered copy, the status line and the messages (also
  where a transport error quotes the URL): the userinfo, the query and the fragment are
  dropped. Scheme, host, port and path are kept byte for byte; a secret inside the path cannot
  be detected. A remembered URL therefore lacks its query: a server that needs one is typed
  again, or opened from a saved configuration.

## Backends

The build picks one by source file:

| Target / file | Backend | Threads |
|---|---|---|
| `pj_websocket_native` → `websocket_client_native.cpp` | IXWebSocket (consumers link `ixwebsocket::ixwebsocket`) | events arrive on IXWebSocket's thread |
| `websocket_client_wasm.cpp` | `<emscripten/websocket.h>`, compiled once by PlotJuggler's WASM build for every consumer | the socket lives on the browser main thread; calls post to it and never wait |
| `pj_websocket_fake` → `testing/fake_websocket_client.cpp` | test double: the test plays the server | single-threaded |

A page served over `https://` can open only `wss://` sockets (except to localhost), so a
plain `ws://` server on a robot needs TLS in front of it for the browser build.
