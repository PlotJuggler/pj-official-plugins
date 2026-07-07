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

## Known Limitations

- No per-topic rate limiting (`max_rate_hz`) — the wire protocol supports it, but
  it is not yet exposed.
- No automatic reconnection: if the connection drops mid-stream the source stops
  rather than retrying.
