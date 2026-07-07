# PlotJuggler Bridge Streaming Source

Connects to a PlotJuggler WebSocket Bridge server for real-time data
streaming using the PJ binary protocol.

## Features

- WebSocket connection with configurable address and port
- Topic discovery with periodic refresh (~1s) while dialog is open
- Case-insensitive topic filter in the dialog (matches name and type)
- Binary frame parsing: ZSTD-compressed multi-message payloads
- Delegated ingest with per-topic parser binding
- Parser options: array size, clamp/skip, embedded timestamp
- Connection-global pause/resume and a ~1 Hz heartbeat keep-alive
- **Demand-driven per-topic subscriptions** (`pj.topic_subscription.v1`) — see below

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
everything.

### Threading

Inbound WebSocket text frames (subscribe responses, `topics_changed`,
`get_topics`) are queued on the socket thread and processed on the poll thread,
so parser bindings are only ever created and read from the poll thread. Binary
data frames are queued the same way. The desired-topic set from the host is held
in a mutex-protected latest-wins slot and drained on the poll thread.

## Test scripts

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
  rather than retrying.
