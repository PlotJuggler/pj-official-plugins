# MCAP Data Loader

Imports [MCAP](https://mcap.dev/) files. Channels are mapped to bindings, schemas are advertised to the matching
MessageParser plugin, and each message is pushed to the host as a **deferred byte fetcher**: the plugin never decodes
content or invokes a parser itself.

## Architecture — declarative `pushMessage` with a fetcher

For every MCAP message the loader passes the host a binding handle (resolved during channel discovery), a nanosecond
timestamp, and a closure that materializes the message bytes on demand:

```cpp
runtimeHost().pushMessage(
    binding_handle, timestamp_ns,
    [reader = reader_, offset = msg.offset]() -> PJ::sdk::PayloadView {
      auto bytes = readMessageBytesAt(reader, offset);
      return PJ::sdk::PayloadView{ bytes_span(bytes), bytes };
    });
```

The closure is idempotent: the host may invoke it zero, one or many times. The `PayloadView` it returns carries a
`Span<const uint8_t>` over the payload and a `BufferAnchor` (`std::shared_ptr<const void>`) that keeps the storage alive
as long as the host or any consumer holds it: the `McapReader`, or a `shared_ptr<vector<uint8_t>>` holding a
decompressed chunk.

The loader does not inspect `ObjectIngestPolicy`, invoke the bound parser, or push to the `ObjectStore`. The host
decides per message whether to fetch, parse and materialize now, parse scalars now and defer the object, or defer
everything as a lazy `ObjectStore` entry fetched only when a consumer pulls.

## Reader lifetime

The `McapReader` is shared with each fetcher closure via `shared_ptr`, so it (and any open chunk) stays alive as long as
a deferred entry remains in the `ObjectStore`. A decompressed chunk reused across many messages is cached behind one
`shared_ptr<vector<uint8_t>>` anchor, so deferred entries share the same zero-copy buffer.

## Channel discovery

On open, the loader reads the MCAP summary (falling back to a scan when it is missing), advertises each schema to the
matching parser, and resolves a binding handle per channel through `runtimeHost().bindChannel(...)`.

The dialog lists the discovered channels with schema and encoding. It reads them through the summary's SummaryOffset
records without parsing the chunk indexes, which can take a second or more on a bag with hundreds of thousands of
chunks. It falls back to the full summary read (which scans the data when the summary is unusable) when the summary has
no SummaryOffset records or no Statistics, or when a group is damaged or incomplete (a record that does not parse,
fewer channels or schemas than the Statistics count).

Per-import options:

- **Topic filter**: space-separated AND matching against channel names.
- **Max array size** + **clamp vs. discard**: forwarded to the bound parser through its configuration envelope.
- **Timestamp source**: publish time vs. log time.
- **Embedded timestamp**: taken from message headers when the parser supports it (e.g. ROS `header.stamp`).

## Split ROS 2 bags (`metadata.yaml`)

Opening a rosbag2 `metadata.yaml` loads every split listed under `relative_file_paths` as ONE dataset: the dialog merges
channels by topic (message counts summed), each topic binds a single parser, and the splits are read back to back in
listed order. Every split keeps its own cold-path byte store, so lazy `ObjectStore` pulls re-read the file the message
came from: all splits must stay in place for the whole session.

Only MCAP-storage bags are accepted. A `sqlite3` bag (`.db3` files) or a bag split with per-file compression
(`.mcap.zstd`) is rejected with a message pointing at `ros2 bag convert`; nothing is half-loaded. A damaged split (e.g.
the last one cut short by a killed recorder) is reported as a partial recovery while the other splits still load.

Bags recorded with per-message compression (`compression_mode: message`) load directly when the format is `zstd`,
whether opened through `metadata.yaml` or as a single `.mcap`. Each file is read according to the `rosbag2` metadata
record it carries. Other per-message formats are rejected before anything loads, with a message pointing at
`ros2 bag convert`.

## Always-included channels

Channels whose `(schema name, encoding)` matches a small built-in whitelist are always loaded, pre-checked and locked
in the picker, regardless of the user's selection: PJ4's Scene3D needs them to place other 3D objects
(`PointCloud`/`Mesh3D`) and silently breaks without them. The whitelist covers ROS 2's transform tree
(`tf2_msgs/msg/TFMessage` over `cdr`) and Foxglove's (`foxglove.FrameTransform` over `protobuf`). The match is on
message type, not topic name, because Foxglove has no fixed topic name the way ROS fixes `/tf`. The whitelist is data
(`PJ::AlwaysIncluded::kRules` in
[`common/always_included`](../common/always_included/include/pj_always_included/always_included.hpp), shared with the
PlotJuggler Bridge, Foxglove Bridge and ROS 2 streamers, with the locked rows' tooltip), so the loader stays
format-agnostic. Unlike the streamers, the loader forces a matching channel only when it holds messages.

## How parsers see this loader

Any `MessageParser` that advertises a `SchemaHandler` for an MCAP channel's schema can consume its messages; the
parsers have no MCAP-specific code path. `parser_ros`, `parser_protobuf`, `parser_json` and `parser_data_tamer` are
wired this way out of the box.
