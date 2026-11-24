# PlotJuggler Bridge Streaming Source

Connects to a PlotJuggler WebSocket Bridge server for real-time data
streaming using the PJ binary protocol.

## Features

- WebSocket connection to a server URL typed in one line (see [Connection settings](#connection-settings))
- Runs on the desktop (IXWebSocket) and in the browser build (`<emscripten/websocket.h>`)
- Topic discovery with periodic refresh (~1s) while dialog is open
- Case-insensitive topic filter in the dialog (matches name and type), Select All / Deselect All
- Transform topics always subscribed (see [Connection settings](#connection-settings))
- Binary frame parsing: ZSTD-compressed multi-message payloads
- Delegated ingest with per-topic parser binding
- Parser options: array size, clamp/skip, embedded timestamp
- Connection-global pause/resume and a ~1 Hz heartbeat keep-alive
- **Demand-driven per-topic subscriptions** (`pj.topic_subscription.v1`) — see below

## Connection settings

| Key | Meaning |
|---|---|
| `url` | The server URL (e.g. `wss://robot.local:8443/bridge`), saved normalized. Loading accepts the same forgiving input as the dialog; an invalid one is rejected. |
| `address`, `port` | Legacy keys, read only when `url` is absent: `ws://<address>:<port>` (default `127.0.0.1:9090`, the bridge server's default port). Never written. |

The dialog takes the server as one URL line (placeholder `ws://127.0.0.1:9090`); a bare host
or `host:port` gets `ws://` and port 9090. Which URLs are accepted or refused (credentials and
fragments are), how the URL is saved, and how the last connected URL is remembered and cleaned
of secrets: [The server URL](../common/websocket/README.md#the-server-url).

After Connect, the dialog retries a refused, unanswered (5 s per attempt) or dropped
connection with a backoff from 1 s up to 10 s until it connects or the user presses Cancel;
the status line under the URL shows connecting, retrying in N s with the last error, or
connected.

**Select All** (Ctrl+A) selects the topics the filter shows; **Deselect All** (Ctrl+Shift+A)
clears every selection, also those the filter hides.

Transform topics are always subscribed, whatever the selection and the host's demand: every
advertised topic of type `tf2_msgs/msg/TFMessage` (the bridge serves ROS 2 topics, all CDR, so
the topic's `encoding` field is not consulted). Their rows are locked and selected, and they are
saved with the selection; the rule, shared with the MCAP loader and the other streamers, is in
[`always_included.hpp`](../common/always_included/include/pj_always_included/always_included.hpp).

To try it, replay the MCAP loader's transform fixture (`/tf`, `/tf_static`, a
Foxglove `transforms` topic that is not forced, `/near_miss_encoding`, which is forced here
because only the type counts, and an ordinary topic): `python3 test_scripts/pj_bridge_mcap_player.py ../data_load_mcap/test_data/test_dialog_whitelist.mcap --loop`.

Starting the stream never waits for the network: `start()` only begins connecting, and
each poll advances the connection from *connecting* (5 s deadline) to *discovery*
(`get_topics` in demand mode, or the legacy `subscribe` and its 5 s response deadline)
to *streaming*. A connection that cannot open, closes, or errors stops the stream with
one error naming the URL; later events never replace that first error. When the dialog
is already connected on OK, the source adopts that connection instead of dialing again.

## Protocol

The PJ Bridge protocol uses JSON for control messages (`get_topics`,
`subscribe`, `unsubscribe`, `subscribe_topic_updates`, `pause`, `resume`,
`heartbeat`) and a custom binary format for data frames. Binary frames have a
4-byte magic (`0x42524A50` = "PJRB"), message count, uncompressed size, flags,
and a ZSTD-compressed payload containing `[topic_len:u16][topic][timestamp_ns:u64]
[cdr_len:u32][cdr]` triples.

`subscribe` is **additive**: it merges the requested topics into the session's
subscription set without disturbing the others. `unsubscribe` drops topics by
name. The subscribe response carries the authoritative per-topic schemas
(`{topic: {encoding, definition}}`) used to bind parsers.

## Demand-driven subscriptions

When connected to a host that supports the `pj.topic_subscription.v1` extension
(advertised via `kCapabilityPerTopicPause`), the source runs in **demand mode**:

1. On start it advertises the full topic catalog to the host via
   `notifyAvailableTopics` — the host lists every topic as a paused placeholder.
2. It sends `get_topics` **and** `subscribe_topic_updates`, both with
   `include_schemas: true`, so the initial catalog and every subsequent
   `topics_changed` push carry per-topic `encoding` + `definition`. Old servers
   that ignore `include_schemas` simply answer name + type — the source still
   works, because the authoritative schema always arrives in the subscribe
   response. A topic whose schema extraction failed appears as name + type only
   and is tolerated.
3. The host pushes the set of topics it wants displayed (`set_active_topics`).
   The source reconciles: it sends one additive `subscribe` for newly wanted
   topics and one `unsubscribe` for topics no longer displayed, so no data flows
   for a topic that isn't being plotted. A desired topic the server hasn't
   advertised yet stays pending and subscribes as soon as `topics_changed`
   announces it.

**Legacy fallback.** Against an old host that lacks the extension,
`notifyAvailableTopics` fails; the source detects this once at start and falls
back to the original behavior exactly: it subscribes to the dialog's selected
topics up front and streams them. In this mode the dialog's topic selection is
the subscribe list. In demand mode that same selection becomes an **optional
advertise filter** — leave it empty to advertise (and let the host subscribe to)
everything. The always-subscribed transforms saved with the selection do not count: a
selection of transforms only is no filter.

### Threading and transport

The transport is the shared [`common/websocket`](../common/websocket/README.md)
client, also used by the Foxglove Bridge: `<pj_websocket/websocket_client.hpp>` is the
contract, and the build picks the backend by source file (`websocket_client_native.cpp`
or `websocket_client_wasm.cpp`). The dialog's connection fields, retry and status line
come from `<pj_websocket/connection_dialog.hpp>`.

- Every transport event (open, text and binary frames, close, error) is copied into
  one bounded FIFO, `PJ::WebSocket::EventQueue`, and processed in arrival order on the poll
  thread (on the dialog's tick while the dialog owns the connection), so parser
  bindings are only ever created and read from the poll thread.
- The queue holds at most 512 MiB of unprocessed data frames on the desktop and 64 MiB
  in the browser. The poll thread applies one drained batch while the queue refills, so
  unprocessed data can peak at about twice that (1 GiB / 128 MiB). When the poll thread falls behind, the OLDEST data frames are dropped
  (the newest is always kept) and one warning reports the running count, repeated at
  most every 5 s; the stream keeps running. Control events (open, text, close, error)
  are never dropped. A PJRB frame is self-contained, so a drop leaves a gap in the plots,
  never a broken stream. A dropped frame of a latched or one-shot topic (`/tf_static`,
  `/robot_description`) is not resent, so that data stays missing until the bridge
  republishes it. The existing 256 MB decompression limit per frame is unchanged.
- In the browser the WebSocket, its callbacks and every send, close and delete run on
  the main runtime thread. The worker never waits for it: `stop()` queues the close
  and returns, because the main thread may be joining that worker.
- `stop()` is idempotent and closes the queue first, so callbacks that land late are
  dropped; each start uses a fresh queue.
- The desired-topic set from the host is held in a mutex-protected latest-wins slot
  and drained on the poll thread once the connection is open.

## Compatibility policy

`protocol_version` (currently 1, stamped on every response) is the ONLY hard
gate: a response from a KNOWN-newer protocol stops the stream with an upgrade
message. Everything additive is feature-detected through the `server`
capability list in the `get_topics` response — a missing capability degrades
that one feature with a precise warning (e.g. no `include_schemas` → topic
classification degraded). Version strings are never compared.

## Latched (transient-local) topics

Latched topics (`/tf_static`, `/map`, …) publish a retained sample that late
subscribers depend on — and under demand-driven subscriptions every subscriber
is late by construction. The protocol makes this the SERVER's job: retained
samples are replayed right after the subscribe response that adds the topic
(the client just ingests them), and topic entries carry an optional
`latched: true` badge when the server knows a topic is transient-local (see
the plotjuggler_bridge docs/API.md contract). Detection in the test servers is
shared (`common/test_support/mcap_qos.py`): recorded `offered_qos_profiles`
channel metadata when present, a name heuristic otherwise, plus the players'
`--latch` override.

## Test scripts

> The mcap player defaults to a SINGLE pass (looping rewinds bag time,
> which live consumers see as non-monotonic). Run selftest data-flow checks
> against it with `--loop`, or use a bag longer than the check run.


`test_scripts/` holds standalone Python servers that speak the PJ Bridge wire
protocol so the plugin (and the self-test below) can be exercised without a real
ROS/DDS bridge. Their JSON control plane mirrors the **production** server
semantics (`github.com/PlotJuggler/plotjuggler_bridge`), so demand-driven
per-topic subscriptions behave identically here and against the real bridge. All
three share the same control-plane rules:

- **`subscribe` is additive** — requested topics are merged into the session's
  subscription set; already-subscribed topics are a no-op (no schema re-echoed).
  Response: `{"status": "success"|"partial_success", "schemas":
  {topic: {encoding, definition}}, "failures": [{topic, reason}]}`. An unknown
  topic fails per-topic with `"Topic does not exist"` (and is forgotten, no
  sticky retry); if **every** requested topic fails the response is
  `{"status": "error", "error_code": "ALL_SUBSCRIPTIONS_FAILED"}`.
- **`unsubscribe`** removes topics from the session set and replies
  `{"status": "success", "removed": [names actually removed]}` (unknown names
  are silently ignored); data for unsubscribed topics stops flowing.
- **`get_topics` honors `include_schemas`** — default (absent/false) entries carry
  only `{name, type}`; `include_schemas: true` adds `{encoding, definition}`.
  (Production always returns name+type only; the servers make schemas *opt-in* so
  both the schema-present and graceful-degradation paths are exercised.)
- **`subscribe_topic_updates` / `unsubscribe_topic_updates`** opt a session in/out
  of `{"notification": "topics_changed", "added": [...], "removed": [...]}`
  pushes (no `id` on notifications; `added` entry shape honors the
  `include_schemas` flag from the opt-in request).
- **`heartbeat`** replies `{"status": "ok"}`; **`pause`/`resume`** reply
  `{"status": "ok", "paused": bool}`.
- Every response echoes the request's `id` (when it is a string) and carries
  `protocol_version: 1`.

Servers:

- **`pj_bridge_test_server.py`** — three synthetic scalars (`/test/sine`,
  `/test/cosine`, `/test/sawtooth`, `json`). `--late-topic NAME:SECONDS`
  advertises one extra topic `SECONDS` after startup and pushes `topics_changed`
  to opted-in sessions (exercises the pushed-advertisement path).
- **`pj_bridge_mixed_server.py`** — scalars plus a `sensor_msgs/CompressedImage`
  JPEG topic (`cdr`). Static topic set: it implements the topic-updates opt-in but
  never pushes (nothing changes).
- **`pj_bridge_mcap_player.py`** — replays any `.mcap` file. Its channel set is
  fully known at open, so it likewise opts in but pushes nothing. The wire
  `encoding` is the channel's `message_encoding` (`cdr`, `json`, …).

**`protocol_selftest.py`** is a standalone WebSocket client that asserts all of
the above end-to-end against a running server and prints a per-check PASS/FAIL/SKIP
matrix (exit nonzero on any failure). It is server-agnostic — it discovers topics
via `get_topics` and exercises the protocol against whatever is advertised:

```bash
# terminal 1
python3 test_scripts/pj_bridge_test_server.py --port 9877 --late-topic /test/late:3
# terminal 2
python3 test_scripts/protocol_selftest.py --port 9877 --expect-late-topic /test/late
```

Without `--expect-late-topic` the `topics_changed` leg reports SKIP (for the
mixed server and the mcap player, which advertise a fixed topic set).

## Known Limitations

- No per-topic rate limiting (`max_rate_hz`) — the wire protocol supports it, but
  it is not yet exposed.
- No automatic reconnection: if the connection drops mid-stream the source stops
  with an error rather than retrying (browsers offer no reconnection either).
