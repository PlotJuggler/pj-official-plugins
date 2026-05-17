# MCAP Data Loader

Imports [MCAP](https://mcap.dev/) files. Channels are mapped to bindings,
schemas are advertised to the matching MessageParser plugin, and each
message is pushed to the host as a **deferred byte fetcher** — the
plugin never decodes content or invokes a parser directly.

## Architecture — declarative `pushMessage` with a fetcher

For every MCAP message the loader passes the host:

- a binding handle (resolved during channel discovery),
- a nanosecond timestamp,
- a **closure that materializes the message bytes on demand**.

```cpp
runtimeHost().pushMessage(
    binding_handle, timestamp_ns,
    [reader = reader_, offset = msg.offset]() -> PJ::sdk::PayloadView {
      // Materialize the bytes on demand. Idempotent — the host may
      // invoke this zero, one, or many times depending on policy and
      // consumer pulls.
      auto bytes = readMessageBytesAt(reader, offset);
      return PJ::sdk::PayloadView{ bytes_span(bytes), bytes };
    });
```

The returned `PJ::sdk::PayloadView` carries:

- `bytes` — a `Span<const uint8_t>` over the message payload.
- `anchor` — a `BufferAnchor` (`std::shared_ptr<const void>`) that keeps
  the underlying storage alive for as long as the host or any downstream
  consumer holds a reference. The loader typically anchors on the
  `McapReader` or on a `shared_ptr<vector<uint8_t>>` holding the
  decompressed chunk.

That is the entire ingest API. The loader does **not**:

- inspect `ObjectIngestPolicy`,
- invoke the bound parser,
- push to the `ObjectStore`.

The host decides, per message, whether to fetch + parse + materialize
now, fetch + parse scalars now and defer the object, or defer the whole
thing as a lazy `ObjectStore` entry whose fetcher is invoked only when a
consumer pulls.

## Reader lifetime

The `McapReader` is owned by the loader and shared with each fetcher
closure via `shared_ptr`. As long as any deferred entry remains in the
`ObjectStore`, the reader (and any chunk it has open) stays alive. When
all references drop, the reader is closed automatically.

For chunked MCAP files where decompressed chunks are reused across many
messages, the loader caches the chunk buffer behind a
`shared_ptr<vector<uint8_t>>` and hands that anchor to every fetcher
that draws bytes from it — multiple deferred entries share the same
zero-copy buffer.

## Channel discovery

On open, the loader reads the MCAP summary (with fallback scan when the
summary is missing), enumerates channels and schemas, advertises each
schema to the matching parser, and resolves a binding handle per channel
through `runtimeHost().bindChannel(...)`.

The dialog lists the discovered channels with schema and encoding info
so the user can select which topics to import. Per-import options:

- **Topic filter** — space-separated AND matching against channel names.
- **Max array size** + **clamp vs. discard** — forwarded to the bound
  parser through its configuration envelope.
- **Timestamp source** — publish time vs. log time.
- **Embedded timestamp** — extract from message headers when supported
  by the parser (e.g. ROS `header.stamp`).

## How parsers see this loader

Because the loader speaks only `pushMessage` with a fetcher, any
`MessageParser` that advertises a `SchemaHandler` for an MCAP channel's
schema can consume its messages — there is no MCAP-specific code path
in the parsers. `parser_ros`, `parser_protobuf`, `parser_json`, and
`parser_data_tamer` are wired this way out of the box.
