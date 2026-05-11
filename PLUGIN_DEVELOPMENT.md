# Plugin Development Guide

Reference guide for authors of DataSource and MessageParser plugins in
this collection. Reads top-down: start with the two plugin families
that live in this repo, then with what they emit (scalars and canonical
objects), then the concrete DataSource and MessageParser shapes, and
finally the end-to-end host dispatch.

## The two plugin families

This collection contains exactly two kinds of plugin. They have
disjoint responsibilities, and the only thing they share is the host
that wires them together.

### DataSource plugins — produce a stream of timestamped messages

A DataSource is the entry point of data into the host. It enumerates
channels, advertises each channel's schema, and emits messages as
`(binding_handle, timestamp_ns, fetcher)` triples — where the
**fetcher** is a small callback the host can invoke later to retrieve
the bytes for that message (think *"give me the data for this instant
in time"*). The detailed shape of the fetcher is covered in the
DataSource concrete section below; for now, the important property is
that the bytes are not handed to the host eagerly — only a way to
obtain them is. There are two sub-shapes:

| Sub-shape | Base class | When to use | Examples |
|-----------|------------|-------------|----------|
| **DataLoader** | `FileSourceBase` | Bounded source: enumerate every message in a file / archive once. | `data_load_csv`, `data_load_mcap`, `data_load_parquet`, `data_load_ulog` |
| **DataStream** | `StreamSourceBase` | Unbounded source: push messages live as they arrive from a socket / WebSocket / broker; report connection state. | `data_stream_zmq`, `data_stream_mqtt`, `data_stream_foxglove_bridge`, `data_stream_pj_bridge` |

"Enumerate" here is lighter than it sounds. The loader walks the file
from start to end and announces every message to the host, but it does
not decide what happens next: the host can read and store each message
right away, can read only the cheap parts (timestamps, small fields)
and leave the heavy payload alone for now, or can keep nothing in
memory at all and go back to the file only when the user actually asks
to see a frame. The loader's job is identical in every case — one
announcement per message — so a multi-gigabyte recording does not have
to fit in memory at load time. The mechanism that makes those choices
is covered further down.

What a DataSource **is not**:

- **Not a decoder.** It does not know what the bytes mean. It
  advertises the schema (type name + raw schema bytes) and the
  appropriate parser does the translation.
- **Not a plotter.** It knows nothing about widgets, rendering, or
  curves.
- **Not storage.** It does not write to the `ObjectStore` or the
  `DataEngine` directly. The host orchestrates persistence according
  to ingest policy.
- **Not policy-aware.** It does not consult `ObjectIngestPolicy`,
  does not branch on eager vs lazy, and does not vary its behaviour
  per topic. One `pushMessage` call per message, regardless.

### MessageParser plugins — translate bytes into columns and canonical objects

A MessageParser is a schema-bound decoder. Given a schema name and a
payload, it produces scalar columns (always) and, when the schema
maps to a media type, a canonical object (optionally).

Examples: `parser_json`, `parser_protobuf`, `parser_ros`,
`parser_data_tamer`.

What a MessageParser **is not**:

- **Not a byte fetcher.** It does not open files, sockets, or
  brokers. It receives bytes from the host.
- **Not topic-aware.** It binds by schema, not by topic. Two
  channels carrying the same schema share a single bound handler.
- **Not format-specific.** It does not know whether the bytes came
  from an MCAP file, a WebSocket, a ROS bag, or a Parquet column —
  only what the schema says.
- **Not the dispatcher.** It does not decide *when* to parse. It
  answers `parse_scalars` / `parse_object` when the host calls,
  and nothing more.

### Why the separation matters

Because DataSources never decode and MessageParsers never fetch, the
two families compose freely. Any DataSource that advertises a known
schema can use any parser that handles it: `parser_ros` decodes ROS
messages whether they arrived through `data_load_mcap`,
`data_stream_foxglove_bridge`, or a future bag streamer — without a
single line of MCAP- or bridge-specific code in the parser.

## What plugins emit: scalars and canonical objects in time

PlotJuggler began life as a time-series plotter. The primary product of
the ingest pipeline has always been — and still is — **scalar columns
extracted from message fields**: every `IMU.angular_velocity.x`,
`JointState.position[3]`, `BatteryState.voltage`, `/cmd_vel.linear.x`
ends up as a stream of `(timestamp, value)` samples that the plotter,
tables, transforms, and Lua scripts can consume directly. Most plugins
in this collection do exactly that and nothing else — `parser_json`,
`parser_protobuf`, `data_load_csv`, `data_load_ulog`, `data_load_parquet`,
all the streaming sources. The canonical mental model of a parser is
*"walk the message, name every primitive, emit one column per leaf"*.

Some message types do not fit the scalar model: a camera frame, a
compressed image, a point cloud. To visualize those, PJ4 adds a second,
narrow ingest channel — **canonical objects**, opaque-to-the-plotter
media payloads that non-scalar widgets decode and render. The image
viewer and the point-cloud viewer are the existing consumers; the same
channel is how later viewers for 2D laser scans, meshes, transformation
trees, or similar non-scalar primitives plug in. A single message can
emit both: scalar metadata (image `width` / `height` / `frame_id` /
`encoding`, point-cloud `point_step` / `row_step`, …) **plus** the
canonical object that carries the raw pixels or point bytes.

These payloads are usually large in aggregate. A 200 GB MCAP recording
of camera streams holds tens of thousands of frames that cannot all sit
in memory at once. For that reason canonical objects are typically
loaded **on demand**: the host keeps the fetcher associated with each
object and invokes it only when something actually asks for the bytes.
Whether ingest is eager, partly eager, or fully on demand is
configurable per topic and per source, but because on-demand is the
common case, every canonical-object channel needs a way to *name the
work to be done later* — which is precisely what the fetcher is. The
DataSource section below details the exact signature; here it is enough
to think of it as "the callback that returns the bytes for one message
when called".

Concretely, every bound schema is described by a `CatalogEntry` with two
output slots:

| Slot | Output channel | Consumer |
|------|----------------|----------|
| `parse_scalars` | Columns through the `ScalarSink` | Plotter, tables, transforms, Lua |
| `parse_object` (optional) | A `CanonicalObject` variant | Image / point-cloud / media viewers |

Scalar-only schemas leave `parse_object` empty; canonical-object schemas
fill both. The rest of this document covers each channel in turn — the
canonical-object vocabulary first, because the scalar path is the
familiar one and the canonical one is what the new SDK pieces are
about.

## Canonical objects — the media vocabulary

A **canonical object** is a small `std::variant` defined in
`pj_base/sdk/canonical_object.h`. It is the *only* shape a parser is
allowed to hand to the visualizers. Today the variant covers:

| Type | Purpose |
|------|---------|
| `sdk::Image` | Raw / uncompressed image — width, height, `PixelFormat`, `row_step`, `Span<const uint8_t> pixels`, `BufferAnchor`. |
| `sdk::CompressedImage` | Already-compressed image — `Format` (JPEG / PNG / QOI / …), `Span<const uint8_t> bytes`, optional `extras` (e.g. `compressed_depth_min/max`), `BufferAnchor`. |
| `sdk::PointCloud` | Point-cloud frame — `width`, `height`, `point_step`, `row_step`, field descriptors, raw `Span<const uint8_t> data`, `BufferAnchor`. |

The variant is closed-by-design — viewer code uses
`std::visit` to dispatch, and `CanonicalObjectKind` (`kNone`, `kImage`,
`kCompressedImage`, `kPointCloud`) is used by parsers to declare which
kind a schema produces *before* the bytes are touched.

### Why "canonical"?

- **Source-agnostic.** A `sensor_msgs/Image` over ROS CDR, a
  Foxglove-bridge image, or a frame extracted from a video container all
  land in the viewer as the same `sdk::Image`. The viewer never learns
  the wire format.
- **Zero-copy by default.** The variant carries `Span<const uint8_t>` +
  `BufferAnchor` (`std::shared_ptr<const void>`), so the parser can
  return spans over the original payload buffer. The host copies into
  the `ObjectStore` only when retention policy demands it.
- **Stable for viewers.** New widgets (e.g. depth fusion, ROIs) only
  add consumers of existing variants. New encodings are handled by
  adding a `Format` value, not by introducing a new viewer-facing type.

### `PayloadView` and `BufferAnchor`

Both the DataSource API and the parser API speak in `PayloadView`:

```cpp
namespace PJ::sdk {
struct PayloadView {
  Span<const uint8_t> bytes;    // the message payload
  BufferAnchor       anchor;    // keeps `bytes` alive
};

using BufferAnchor = std::shared_ptr<const void>;
}
```

`BufferAnchor` is an opaque ownership token. Anything that satisfies
`shared_ptr<T>` works (a `shared_ptr<vector<uint8_t>>`, a
`shared_ptr<SomeReader>` holding the chunk, a custom `shared_ptr<void>`
with a deleter). The host and downstream consumers keep this anchor
alive for as long as they reference the spans built from it.

## DataSource plugins — declarative `pushMessage` with a fetcher

A DataSource does not deliver bytes. It delivers a **callable that
produces bytes when invoked**. One call per message, regardless of the
host's ingest policy:

```cpp
runtimeHost().pushMessage(
    binding_handle, timestamp_ns,
    [reader = reader_shared_ptr_, offset = msg.offset]() -> PJ::sdk::PayloadView {
      // Materialize the bytes on demand. Idempotent — the host may
      // invoke this zero, one, or many times depending on the active
      // policy and consumer pulls.
      return readMessageBytesAt(reader, offset);
    });
```

The fetcher closure may return:

- `PJ::sdk::PayloadView { bytes, anchor }` — preferred. Zero-copy
  `Span<const uint8_t>` over a buffer the plugin keeps alive via
  `BufferAnchor` (typically a `shared_ptr<vector<uint8_t>>` referencing
  the source's chunk/page, or a `shared_ptr` to the reader itself).
- `std::vector<uint8_t>` — convenience overload. The SDK template
  heap-allocates the vector and treats it as its own anchor.

What the DataSource **does NOT** do:

- It does **not** consult ingest policy. The host applies `kEager`,
  `kLazyObjectsEagerScalars`, or `kPureLazy` per-message via an
  `ObjectIngestPolicyResolver`.
- It does **not** invoke the parser. The host does, when (and only
  when) it is the right time.
- It does **not** push to the `ObjectStore` directly. The host
  orchestrates that.

This shape lets the same `pushMessage` call result in either an
immediate parse + store, or a deferred entry that the host materializes
only when a consumer pulls — without the plugin caring.

**Reference implementation:** `data_load_mcap` — the fetcher closure
captures the open `mcap::McapReader` (as a `shared_ptr`) plus the
message offset, and reads the bytes on demand.

## MessageParser plugins — declarative `SchemaHandler` catalog

A parser does not override `parse()`. It declares a **table of
handlers**, one entry per schema type name it knows how to translate.
The optional default entry (`CatalogEntry::kDefault`, conventionally
keyed as `"*"`) declares the generic fallback used for every schema not
matched by name:

```cpp
// In your plugin's class scope: a static catalog of schemas. Pure
// data — member-function pointers, no `this` capture.
const auto& MyParser::catalog() {
  using Kind = PJ::sdk::CanonicalObjectKind;
  static const std::unordered_map<std::string, CatalogEntry> kMap = {
      // Canonical-object schema: produces an sdk::Image /
      // CompressedImage / PointCloud via parse_object, plus
      // small-metadata scalars via parse_scalars.
      {"my_pkg/MyImage",
          {.object_kind   = Kind::kImage,
           .parse_scalars = &MyParser::imageScalars,
           .parse_object  = &MyParser::parseImage}},

      // Scalar-only schema: just emits columns.
      {"my_pkg/MyTelemetry",
          {.parse_scalars = &MyParser::telemetryScalars}},

      // Default entry — generic fallback for every other schema.
      // Optional: omit it and any unmatched schema is rejected at
      // bindSchema time.
      {CatalogEntry::kDefault,
          {.parse_scalars = &MyParser::genericScalars}},
  };
  return kMap;
}

// `bindSchema` becomes a single lookup with no branching: resolve()
// returns the exact-match entry if present, otherwise the default.
PJ::Status MyParser::bindSchema(std::string_view type_name,
                                 PJ::Span<const uint8_t> schema) {
  base::bindSchema(type_name, schema);
  // ... parser-specific setup ...

  registerSchemaHandler(type_name,
                        makeHandler(catalog().resolve(type_name)));
  return PJ::Status::ok();
}
```

`CatalogEntry` carries:

- `object_kind` — `CanonicalObjectKind::kImage` /
  `kCompressedImage` / `kPointCloud` / `kNone`. The host uses this to
  pick the right ingest policy *before* the bytes are touched.
- `parse_scalars` — function pointer that walks the payload and emits
  columns through the `ScalarSink`.
- `parse_object` — function pointer that builds the canonical-object
  variant from the payload, populating spans over the input buffer
  whenever possible.

`CatalogEntry::kDefault` is conventionally keyed as `"*"`. A catalog
may omit it (then `resolve()` reports an error for unmatched schemas)
or provide it (then unmatched schemas flow into the generic handler).

The SDK base class implements `classifySchema`, `parseScalars`, and
`parseObject` as table lookups. There is no enum to maintain, no switch
to extend, and no virtual methods to override — **adding a schema is a
new entry in the catalog and the corresponding member-function**.

**Reference implementation:** `parser_ros` — canonical-object handlers
for `sensor_msgs/Image`, `sensor_msgs/CompressedImage`,
`sensor_msgs/PointCloud2`, plus specialized scalar handlers for `Imu`,
`JointState`, `Pose`, `Twist`, `TF2`, `DiagnosticArray`, …

## How the host uses these declarations

**As a plugin author you do not need to write any code that handles
this.** The plugin's job is the declarative shape covered above:
announce messages with a fetcher, declare a schema catalog, answer
honestly when called. The rest of this section explains what the host
does with those declarations, so you have a mental model of *why* the
shape is what it is — not because you have to manage it.

PJ4's runtime is free to pick its ingest strategy per message:

| Policy | Effect |
|--------|--------|
| **`kEager`** | Invoke the fetcher now. Invoke `parseScalars` + `parseObject` now. Copy canonical bytes into the `ObjectStore`. |
| **`kLazyObjectsEagerScalars`** | Invoke the fetcher now. Invoke `parseScalars` now (columns available immediately). Defer the fetcher + `parseObject` behind a lazy `ObjectStore` entry, pulled on demand. |
| **`kPureLazy`** | Skip the fetcher and the parser at push time. Register the fetcher in the `ObjectStore`; nothing runs until a consumer pulls. |

The selection is done by an `ObjectIngestPolicyResolver` that cascades
`topic > source > kind > default`, configured by the runtime. In PJ4
this will eventually be user-facing — per dataset, per topic, per kind
— but the plugin contract does not change when those controls land. A
plugin written today against the declarative shape will keep working
unmodified when the user starts flipping policies in a future PJ4
release.

### What this means in practice for the plugin

Even though you do not touch policy code, the existence of lazy modes
constrains how you implement the plugin:

- **Keep the source open and seekable for as long as the host might
  call your fetcher.** For a DataLoader, that usually means holding
  the file handle / reader as a `shared_ptr` and capturing it inside
  every fetcher closure (and into the `BufferAnchor` if the bytes are
  spans over a mapped or cached chunk). The host may invoke your
  fetcher seconds, minutes, or hours after `pushMessage` returned —
  and in any order, because the user may scrub through time. The
  specifics depend on the underlying technology (mmap, an indexed
  reader, a chunked decompressor, …), but the contract is the same:
  *if your closure is invoked, it must succeed*.

- **Do not cache decoded data inside the plugin.** The entire
  motivation for lazy ingest is to handle datasets that do not fit in
  memory — a 200 GB MCAP recording of camera streams, a multi-hour
  bag of lidar frames. If the plugin holds onto decoded frames or
  duplicates payload bytes "to be helpful", it defeats the point. Let
  the host's `ObjectStore` be the single owner of materialized data;
  the plugin keeps only what it strictly needs to *fetch* bytes again
  on demand (file offsets, chunk indices, the reader itself).

- **Make the fetcher idempotent.** The host may invoke it zero, one,
  or many times for the same message — once at eager-time and again
  later when a viewer pulls; multiple times across a session if the
  store evicts and refetches. Returning the same bytes each call
  (modulo a fresh anchor) is fine; doing one-shot work that the
  closure cannot repeat is not.

## End-to-end dispatch

```
DataSource          Host (PJ4)                MessageParser         ObjectStore
─────────           ──────────                ─────────────         ───────────
pushMessage(ts,
   fetcher)   ──►   resolver.resolve(...)
                      ▼
                    kEager ─► fetcher.fetch()
                              parser.parseScalars  ─►  schemaHandler.parse_scalars
                              parser.parseObject   ─►  schemaHandler.parse_object
                              pushOwned(bytes)                              ─► [bytes]

                    kLazyObjectsEagerScalars ─► fetcher.fetch()
                                                parser.parseScalars  ─►  schemaHandler.parse_scalars
                                                pushLazy(fetcher_closure)              ─► [fetcher]
                                                                  (pull later)         ─► fetcher.fetch + parseObject

                    kPureLazy ─► pushLazy(fetcher_closure)                              ─► [fetcher]
                                                                  (pull later)         ─► fetcher.fetch + parseObject
```

## Authoring checklist

For a new DataSource:

1. Subclass `FileSourceBase` (for file importers) or `StreamSourceBase`
   (for streaming sources).
2. In the open / start flow, advertise each channel's schema to the
   matching parser and resolve a binding handle through
   `runtimeHost().bindChannel(...)`.
3. For each message, call `runtimeHost().pushMessage(...)` with a
   fetcher closure that returns a `PayloadView` over your buffer (use
   a `BufferAnchor` so the bytes stay alive past the call).
4. Do not call the parser, do not consult policy, do not touch the
   `ObjectStore`.

For a new MessageParser:

1. Subclass `MessageParserPluginBase` (or your family's intermediate
   base).
2. Add a static `catalog()` returning the `CatalogEntry` table. Add a
   `CatalogEntry::kDefault` entry if the plugin should fall back to a
   generic handler for unknown schemas; omit it to reject unmatched
   types at bind time.
3. Implement the `parse_scalars` / `parse_object` member functions
   referenced by the catalog. Prefer spans over the input payload;
   carry the input `BufferAnchor` into the canonical object.
4. In `bindSchema`, call
   `registerSchemaHandler(type_name, makeHandler(catalog().resolve(type_name)))`.

## Reference implementations in this repo

| Plugin | Role | What to read it for |
|--------|------|---------------------|
| `data_load_mcap` | DataSource | Deferred fetcher closure capturing the open `McapReader`; reusing decompressed chunks as `BufferAnchor`. |
| `parser_ros` | MessageParser | Static catalog with three canonical-object handlers (Image, CompressedImage, PointCloud2) and many scalar handlers; default introspection fallback. |
