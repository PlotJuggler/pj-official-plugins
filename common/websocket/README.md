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
  `Endpoint` (address or full `ws://`/`wss://` URL, port, transport indicator, config keys)
  and `DialogConnection` (connect/cancel, per-attempt deadline, doubling capped retry
  backoff, status line, handoff of the live connection to the source on OK).
- `<pj_websocket/source_connection.hpp>`: the connection half of a streaming source.
  `SourceConnection` adopts the dialog's open connection or dials a fresh one, enforces the
  connect deadline, latches the first stream-stopping failure (`poll()` returns it until
  `stop()`) and, when `reconnect` is set, re-dials a lost connection on a fixed delay. Frames
  reach the owner through handlers in arrival order, on the poll thread.

## Backends

The build picks one by source file:

| Target / file | Backend | Threads |
|---|---|---|
| `pj_websocket_native` → `websocket_client_native.cpp` | IXWebSocket (consumers link `ixwebsocket::ixwebsocket`) | events arrive on IXWebSocket's thread |
| `websocket_client_wasm.cpp` | `<emscripten/websocket.h>`, compiled once by PlotJuggler's WASM build for every consumer | the socket lives on the browser main thread; calls post to it and never wait |
| `pj_websocket_fake` → `testing/fake_websocket_client.cpp` | test double: the test plays the server | single-threaded |

A page served over `https://` can open only `wss://` sockets (except to localhost), so a
plain `ws://` server on a robot needs TLS in front of it for the browser build.
