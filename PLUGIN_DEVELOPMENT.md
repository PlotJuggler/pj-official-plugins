# Plugin Development Guide

Reference guide for authors of **DataSource** and **MessageParser** plugins
in this collection. Reads top-down: start with the three plugin shapes
that live in this repo and the call sites that distinguish them, then
with what a MessageParser emits (scalars and builtin objects), then a
worked example of how one parser is shared across three sources, and
finally the end-to-end host dispatch.

## The three plugin shapes

Inside this collection, every plugin is one of three shapes. The split
is visible in the plugin code itself — DataSources declare which kind
they are through the capability flag returned from
`extraCapabilities()` — and the three shapes have genuinely different
responsibilities. They are not interchangeable; picking the wrong
shape for a new plugin will fight the runtime the entire way.

| Shape | What it does | Identifier in code | Examples |
|-------|--------------|---------------------|----------|
| **Self-parsing DataSource** | Reads *and* decodes its file/stream in one plugin. Writes scalar fields straight into the host via `writeHost().appendRecord(...)`. | `kCapabilityDirectIngest` | `data_load_ulog`, `data_load_csv`, `data_load_parquet` |
| **Delegating DataSource** | Reads the file/stream, advertises each channel's schema, and hands raw bytes off via `pushMessage`. Never inspects message content. | `kCapabilityDelegatedIngest` | `data_load_mcap`, `data_stream_zmq`, `data_stream_mqtt`, `data_stream_foxglove_bridge` |
| **MessageParser** | Has no I/O of its own. Decodes bytes on behalf of a delegating source whenever the host calls. | Declares its `"encoding"` in the plugin manifest | `parser_protobuf`, `parser_ros`, `parser_json`, `parser_data_tamer` |

You can see the flag at the top of each DataSource class:

```cpp
// data_load_ulog/ulog_source.cpp:
uint64_t extraCapabilities() const override {
  return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
}

// data_load_mcap/mcap_source.cpp:
uint64_t extraCapabilities() const override {
  return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog;
}
```

**When to bundle (self-parsing) vs delegate.** Bundle when the format
is self-describing and has exactly one decoder library that owns it —
ULog with `ulog_cpp`, Parquet with Arrow, CSV with a tiny in-tree
parser. Splitting those into source + parser would just add plumbing
without a payoff. Delegate the moment the container is polyglot (MCAP
carries ROS *or* protobuf *or* flatbuffers, depending on the channel)
or the transport has no schema of its own and the user picks the
encoding (ZMQ and MQTT both default to `"json"`, configurable per
topic).

### Shape A — Self-parsing DataSource (direct ingest)

A self-parsing DataSource owns the whole pipeline for its file format.
It opens the file, understands its container, decodes the contents,
and writes named scalar values straight into the host's data engine
via `writeHost().appendRecord(...)`. No MessageParser is ever
involved.

In the ULog loader, the entire decode-and-write loop is just a handful
of calls:

```cpp
// data_load_ulog/ulog_source.cpp (abridged)
auto topic = writeHost().ensureTopic(topic_name);
for (size_t fi = 0; fi < field_names.size(); ++fi) {
  writeHost().ensureField(*topic, field_names[fi], field_types[fi]);
}

for (const auto& raw_sample : sub->rawSamples()) {
  // ulog_cpp gives us the raw record; extract scalar values by walking the format
  extractFlatValues(raw_sample.data().data(), 0, *sub->format(), values);

  row_fields.clear();
  for (size_t j = 0; j < values.size(); ++j) {
    row_fields.push_back({.name = field_names[j], .value = values[j]});
  }

  writeHost().appendRecord(*topic, PJ::Timestamp{ts_ns},
                           PJ::Span<const PJ::sdk::NamedFieldValue>(row_fields));
}
```

Notice what is **not** there: no `ensureParserBinding`, no
`pushMessage`, no schema advertisement. The plugin never produces a
payload anyone else has to decode. `data_load_csv` and
`data_load_parquet` use the same `writeHost().appendRecord(...)`
pattern with their own decoder libraries.

### Shape B — Delegating DataSource

A delegating DataSource is the entry point for data whose meaning the
source does not understand. It enumerates channels, advertises each
channel's schema and encoding to the host, and emits messages as
`(binding_handle, timestamp_ns, payload)` — where "payload" is either
a FetchMessageData closure (for files) or raw bytes (for streams). The
bytes are never decoded by this plugin; the host routes them to a
MessageParser. There are two sub-shapes, picked by base class:

| Sub-shape | Base class | Ingest call | Examples |
|-----------|------------|-------------|----------|
| **DataLoader** | `FileSourceBase` | `pushMessage(handle, ts, fetchMessageData)` — lazy callable | `data_load_mcap` |
| **DataStream** | `StreamSourceBase` | `pushMessage(handle, ts, fetchMessageData)` — closure returns the just-received bytes (resolved eagerly) | `data_stream_zmq`, `data_stream_mqtt`, `data_stream_foxglove_bridge`, `data_stream_pj_bridge` |

"Enumerate" here is lighter than it sounds. A loader walks the file
from start to end and announces every message to the host, but does
not decide what happens next: the host can read and store each message
right away, can read only the cheap parts and leave the heavy payload
alone, or can keep nothing in memory and go back to the file only when
the user asks to see a frame. The loader's job is identical in every
case — one announcement per message — so a multi-gigabyte recording
does not have to fit in memory at load time. The mechanism that makes
those choices is covered further down. Streams are simpler: there is
no past to seek into, so the bytes arrive eagerly and the host parses
them on the spot.

**What a delegating DataSource is *not*:**

- **Not a decoder.** It does not know what the bytes mean. It
  advertises the schema (type name + raw schema bytes + encoding name)
  and the appropriate parser does the translation.
- **Not a plotter.** It knows nothing about widgets, rendering, or
  curves.
- **Not storage.** It does not write to the `ObjectStore` or the
  `DataEngine` directly. The host orchestrates persistence according
  to ingest policy.
- **Not policy-aware.** It does not consult `ObjectIngestPolicy`,
  does not branch on eager vs lazy, and does not vary its behaviour
  per topic. One `pushMessage` call per message, regardless.

### Shape C — MessageParser

A MessageParser is a schema-bound decoder. Given a schema name and a
payload, it produces scalar columns (always) and, when the schema
maps to a media type, a builtin object (optionally). It owns no I/O —
it never opens a file, a socket, or a broker. The host hands it bytes
and asks for columns; that's the whole contract.

Examples: `parser_json`, `parser_protobuf`, `parser_ros`,
`parser_data_tamer`.

**What a MessageParser is *not*:**

- **Not a byte source.** It does not open files, sockets, or brokers.
  It receives bytes from the host.
- **Not topic-aware.** It binds by schema, not by topic. Two channels
  carrying the same schema share a single bound handler.
- **Not format-specific.** It does not know whether the bytes came
  from an MCAP file, a WebSocket, a ROS bag, or a future bag streamer
  — only what the schema says.
- **Not the dispatcher.** It does not decide *when* to parse. It
  answers `parse_scalars` / `parse_object` when the host calls, and
  nothing more.

### Why this split matters

Self-parsing is the right answer when a format is bonded to exactly
one decoder — every line of plumbing between source and parser would
be friction without a payoff. Delegating is the right answer the
moment a format admits more than one payload type: MCAP, ZMQ, MQTT,
and bridges all carry whatever the user chooses to put in them, and a
parser-per-source design would explode into N × M plugins. The split
lets one `parser_ros` decode ROS messages whether they arrived through
`data_load_mcap`, `data_stream_foxglove_bridge`, or a future bag
streamer — without a single line of MCAP- or bridge-specific code in
the parser. The next section makes the same point concretely with
protobuf.

## What a MessageParser emits: scalars and builtin objects in time

PlotJuggler began life as a time-series plotter. The primary product
of a parser has always been — and still is — **scalar columns
extracted from message fields**: every `IMU.angular_velocity.x`,
`JointState.position[3]`, `BatteryState.voltage`, `/cmd_vel.linear.x`
ends up as a stream of `(timestamp, value)` samples that the plotter,
tables, transforms, and Lua scripts can consume directly. Most parsers
in this collection do exactly that and nothing else — `parser_json`,
`parser_protobuf`, `parser_data_tamer`. The mental model is *"walk
the message, name every primitive, emit one column per leaf"*.

Some message types do not fit the scalar model: a camera frame, a
compressed image, a point cloud. To visualize those, PJ4 adds a
second, narrow ingest channel — **builtin objects**,
opaque-to-the-plotter media payloads that non-scalar widgets decode
and render. The image viewer and the point-cloud viewer are the
existing consumers; the same channel is how later viewers for 2D laser
scans, meshes, transformation trees, or similar non-scalar primitives
plug in. A single message can emit both: scalar metadata (image
`width` / `height` / `frame_id` / `encoding`, point-cloud
`point_step` / `row_step`, …) **plus** the builtin object that
carries the raw pixels or point bytes.

These payloads are usually large in aggregate. A 200 GB MCAP recording
of camera streams holds tens of thousands of frames that cannot all
sit in memory at once. For that reason builtin objects are typically
loaded **on demand**: the host keeps the FetchMessageData callable
associated with each object and invokes it only when something
actually asks for the bytes. Whether ingest is eager, partly eager, or
fully on demand is configurable per topic and per source, but because
on-demand is the common case, every builtin-object channel needs a
way to *name the work to be done later* — which is precisely what the
FetchMessageData callable is. The DataSource section below details
the exact signature; here it is enough to think of it as "the callback
that returns the bytes for one message when called".

Concretely, every bound schema is described by a `CatalogEntry` with
two output slots:

| Slot | Output channel | Consumer |
|------|----------------|----------|
| `parse_scalars` | Columns through the `ScalarSink` | Plotter, tables, transforms, Lua |
| `parse_object` (optional) | A `BuiltinObject` (`std::any`) | Image / depth / point-cloud / annotation viewers |

Scalar-only schemas leave `parse_object` empty; builtin-object schemas
fill both. The rest of this document covers each channel in turn —
the builtin-object vocabulary first, because the scalar path is the
familiar one and the builtin one is what the new SDK pieces are
about.

## Builtin objects — the media vocabulary

A **builtin object** is a type-erased value defined in
`pj_base/builtin/BuiltinObject.hpp`:

```cpp
namespace PJ::sdk {
using BuiltinObject = std::any;
[[nodiscard]] BuiltinObjectType typeOf(const BuiltinObject& obj) noexcept;
}  // namespace PJ::sdk
```

It is the *only* shape a parser is allowed to hand to the
visualizers. The SDK ships one header per concrete builtin type under
`pj_base/builtin/`:

| Type | Purpose |
|------|---------|
| `sdk::Image` | Image — `width`, `height`, `std::string encoding` (`"rgb8"`, `"bgr8"`, `"mono8"`, `"jpeg"`, `"png"`, `"compressedDepth"`, …), `row_step`, `Span<const uint8_t> data`, `BufferAnchor`. The encoding string distinguishes raw vs compressed; the field replaces the old `PixelFormat` enum and unifies what used to be two separate types. |
| `sdk::DepthImage` | Depth image — pixels + camera intrinsics (`K` 3×3, `D` distortion, `distortion_model` string). `R` and `P` matrices are derived via free helpers in `depth_image_utils.hpp`. |
| `sdk::PointCloud` | Point-cloud frame — `width`, `height`, `point_step`, `row_step`, field descriptors, raw `Span<const uint8_t> data`, `BufferAnchor`. |
| `sdk::ImageAnnotations` | 2D vector overlays (points, lines, circles, text). Small enough to own outright (`std::vector` members), no anchor needed. |
| `sdk::FrameTransforms` | Named 3D frame relationships. |

Choosing `std::any` over `std::variant` is deliberate: adding a new
builtin object type no longer changes the public type of `BuiltinObject`, so
plugins built against an older SDK keep producing the types they know
and hosts built against an older SDK simply see
`BuiltinObjectType::kNone` from `typeOf` for unknown types. The
declared type in the catalog (`object_type` below) lets the host pick
an ingest policy *before* the bytes are touched.

Consumers recover the concrete type with `std::any_cast`:

```cpp
auto obj = parser->parseObject(timestamp_ns, payload);
if (obj && PJ::sdk::typeOf(*obj) == PJ::sdk::BuiltinObjectType::kImage) {
  const auto* img = std::any_cast<PJ::sdk::Image>(&*obj);
  // ... use img->encoding, img->data, img->anchor, ...
}
```

### Why "builtin"?

- **Source-agnostic.** A `sensor_msgs/Image` over ROS CDR, a
  Foxglove-bridge image, or a frame extracted from a video container
  all land in the viewer as the same `sdk::Image`. The viewer never
  learns the wire format — only the `encoding` string distinguishes
  raw from compressed.
- **Zero-copy by default.** The buffer-backed types carry
  `Span<const uint8_t>` + `BufferAnchor`
  (`std::shared_ptr<const void>`), so the parser can return spans over
  the original payload buffer. The host copies into the `ObjectStore`
  only when retention policy demands it.
- **Stable for viewers.** New widgets (e.g. depth fusion, ROIs) only
  add consumers of existing builtins. New encodings are handled by
  setting a new `encoding` string on `sdk::Image`, not by introducing
  a new viewer-facing type. New builtin kinds are appended without
  breaking older plugins or hosts.

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
`shared_ptr<SomeReader>` holding the chunk, a custom
`shared_ptr<void>` with a deleter). The host and downstream consumers
keep this anchor alive for as long as they reference the spans built
from it.

## Delegating DataSources — the two ingest calls

Once a DataSource has bound its channels with
`ensureParserBinding(...)`, every message it discovers is announced
through one of two host calls. Which one to use is determined by the
underlying transport, not by policy — files use the lazy form;
streams use the eager form.

| Call | Used by | Payload form | Why |
|------|---------|--------------|-----|
| `pushMessage(handle, ts, fetchMessageData)` | File sources (`data_load_mcap`, future bag streamers) | Closure that returns `PayloadView` when invoked | Bytes are reachable on demand — the file is still there. Pairs with lazy ingest policies; the host can invoke the callable zero, one, or many times. |
| `pushMessage(handle, ts, fetchMessageData)` | Streams (`data_stream_zmq`, `data_stream_mqtt`, bridges) | Closure that returns the just-received payload (own a copy) | Bytes exist only at receive time — no replay. The host resolves the closure eagerly (`kEager`). |

### File source — `pushMessage` with a FetchMessageData callable

A DataLoader does not deliver bytes. It delivers a **callable that
produces bytes when invoked**. One call per message, regardless of the
host's ingest policy:

```cpp
// data_load_mcap/mcap_source.cpp (abridged)
auto push_status = runtimeHost().pushMessage(
    binding_it->second, timestamp_ns,
    [reader = reader_keeper_, off = msg_view.messageOffset,
     ch = msg_view.channel->id, lt = msg_view.message.logTime]() {
      // Materialize the bytes on demand. Idempotent — the host may
      // invoke this zero, one, or many times depending on the active
      // policy and consumer pulls.
      return readMessageBytesAt(reader, off, ch, lt);
    });
```

The closure may return:

- `PJ::sdk::PayloadView { bytes, anchor }` — preferred. Zero-copy
  `Span<const uint8_t>` over a buffer the plugin keeps alive via
  `BufferAnchor` (typically a `shared_ptr<vector<uint8_t>>`
  referencing the source's chunk/page, or a `shared_ptr` to the
  reader itself).
- `std::vector<uint8_t>` — convenience overload. The SDK template
  heap-allocates the vector and treats it as its own anchor.

C ABI counterpart: `PJ_message_data_fetcher_t { ctx,
fetchMessageData, release }` in `pj_base/data_source_protocol.h`. The
C++ template wraps the closure into that struct; the host releases
the context exactly once when the callable is no longer needed.
`fetchMessageData` MUST be thread-safe — the host may invoke it from
the ingest thread (kEager) or from consumer threads (lazy pulls).

### Stream source — `pushMessage` over received bytes

A DataStream wraps the just-arrived bytes in a closure that returns
them. The data is in a receive buffer the next frame will overwrite,
so the closure owns a copy. The host applies the same binding-handle
routing as for files; the only difference is that there is no past to
seek, so the host resolves the closure eagerly at call time.

```cpp
// data_stream_zmq/zmq_source.cpp (abridged)
auto status = runtimeHost().pushMessage(
    it->second, PJ::Timestamp{timestamp_ns},
    [bytes = std::vector<uint8_t>(payload_data, payload_data + payload_size)]() { return bytes; });
```

`data_stream_mqtt` and the bridges use the same call. Streams
therefore see only the "eager" leg of the dispatch tree below:
`kEager` is effectively the only policy that applies to them.

**What the DataSource does *not* do (either call):**

- It does **not** consult ingest policy. The host applies `kEager`,
  `kLazyObjectsEagerScalars`, or `kPureLazy` per-message via an
  `ObjectIngestPolicyResolver`.
- It does **not** invoke the parser. The host does, when (and only
  when) it is the right time.
- It does **not** push to the `ObjectStore` directly. The host
  orchestrates that.

Either call lets the same announcement result in either an immediate
parse + store, or a deferred entry that the host materializes only
when a consumer pulls (lazy file path only) — without the plugin
caring.

**Reference implementations:**
- `data_load_mcap` — the FetchMessageData closure captures the open
  `mcap::McapReader` (as a `shared_ptr`) plus the message offset, and
  reads the bytes on demand.
- `data_stream_zmq` / `data_stream_mqtt` — eager `pushMessage` whose
  closure returns the payload pulled off the socket / broker queue.

## MessageParser plugins — declarative `SchemaHandler` catalog

A parser does not override `parse()`. It declares a **table of
handlers**, one entry per schema type name it knows how to translate.
The optional default entry (`CatalogEntry::kDefault`, conventionally
keyed as `"*"`) declares the generic fallback used for every schema
not matched by name:

```cpp
// In your plugin's class scope: a static catalog of schemas. Pure
// data — member-function pointers, no `this` capture.
const auto& MyParser::catalog() {
  using ObjectType = PJ::sdk::BuiltinObjectType;
  static const std::unordered_map<std::string, CatalogEntry> kMap = {
      // Builtin-object schema: produces an sdk::Image / DepthImage /
      // PointCloud / ImageAnnotations via parse_object, plus
      // small-metadata scalars via parse_scalars.
      {"my_pkg/MyImage",
          {.object_type   = ObjectType::kImage,
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

- `object_type` — `BuiltinObjectType::kImage` / `kDepthImage` /
  `kPointCloud` / `kImageAnnotations` / `kFrameTransforms` / `kNone`. The host uses this
  to pick the right ingest policy *before* the bytes are touched.
- `parse_scalars` — function pointer that walks the payload and emits
  columns through the `ScalarSink`.
- `parse_object` — function pointer that builds the builtin-object
  value from the payload, populating spans over the input buffer
  whenever possible.

`CatalogEntry::kDefault` is conventionally keyed as `"*"`. A catalog
may omit it (then `resolve()` reports an error for unmatched schemas)
or provide it (then unmatched schemas flow into the generic handler).

The SDK base class implements `classifySchema`, `parseScalars`, and
`parseObject` as table lookups. There is no enum to maintain, no
switch to extend, and no virtual methods to override — **adding a
schema is a new entry in the catalog and the corresponding
member-function**.

**Reference implementation:** `parser_ros` — builtin-object handlers
for `sensor_msgs/Image` and `sensor_msgs/CompressedImage` (both
producing the unified `sdk::Image` via the `encoding` string) and
`sensor_msgs/PointCloud2`, plus specialized scalar handlers for
`Imu`, `JointState`, `Pose`, `Twist`, `TF2`, `DiagnosticArray`, …

## Worked example — one parser, three sources

The decoupling claim — "any source × any parser" — is easy to
handwave. Here is what it looks like in the actual code.
`parser_protobuf` is bound today from **three different DataSources**
with completely different transports: an MCAP file, a ZMQ socket, and
an MQTT broker. None of them includes a protobuf header. The parser,
in turn, includes nothing from any of them.

### How the host knows which parser to call

Every MessageParser plugin declares the encoding names it handles in
its manifest. For `parser_protobuf`:

```json
{
  "id": "protobuf-parser",
  "name": "Protobuf Parser",
  "category": "message_parser",
  "encoding": ["protobuf"]
}
```

That single string — `"protobuf"` — is the only thing a DataSource
needs to know. When it calls `ensureParserBinding(...)` with
`parser_encoding = "protobuf"`, the host looks up the registered
parser whose manifest claims that encoding and binds it. The
DataSource never references `ProtobufParser` by class name; the
parser never references MCAP, ZMQ, or MQTT. The encoding string is
the routing key.

### The binding call, three ways

Each source builds the same `ParserBindingRequest` struct. What
differs is where it gets the fields from — the file? the user? the
broker?

**MCAP — schema and encoding come from the file itself.** MCAP files
carry one or more schema records and tag each channel with its
encoding:

```cpp
// data_load_mcap/mcap_source.cpp (abridged)
PJ::ParserBindingRequest request{
    .topic_name = channel_ptr->topic,
    .parser_encoding = encoding,          // e.g. "protobuf" — from MCAP channel
    .type_name = schema->name,            // e.g. "my_pkg.MyMessage" — from MCAP schema
    .schema = schema_bytes,               // FileDescriptorSet bytes — from MCAP schema
    .parser_config_json = parser_config_str,
};
auto handle = runtimeHost().ensureParserBinding(request);
```

**ZMQ — encoding comes from the user, no schema.** A ZMQ socket has
no notion of "schema": it's a stream of bytes the operator chose to
send. So the dialog asks for an encoding (default `"json"`) and the
binding goes out with an empty schema. The parser must have been
pre-configured — for protobuf, that means the operator pasted a
compiled `FileDescriptorSet` into the parser's own dialog before the
stream started:

```cpp
auto binding = runtimeHost().ensureParserBinding({
    .topic_name = topic_name,
    .parser_encoding = default_encoding_,   // e.g. "protobuf" — from user dialog
    .type_name = {},                         // empty — stream doesn't know
    .schema = {},                            // empty — pre-configured on the parser
    .parser_config_json = {},
});
```

**MQTT — same shape, different transport.** Identical pattern. The
encoding still comes from the dialog; the schema is still empty. The
only thing that changes is where `msg.topic` originated (an MQTT
broker publish vs. a ZMQ subscription) — completely invisible to the
parser.

**The pattern generalises.** Swap `"protobuf"` for `"ros1msg"` /
`"cdr"` and the same three sources can talk to `parser_ros`. Swap
for `"json"` and they reach `parser_json`. Adding a future
`data_stream_kafka` requires no changes to any parser; adding a
future `parser_flatbuffers` requires no changes to any source.

## How the host uses these declarations

**As a plugin author you do not need to write any code that handles
this.** The plugin's job is the declarative shape covered above:
announce messages with a FetchMessageData callable, declare a schema
catalog, answer honestly when called. The rest of this section
explains what the host does with those declarations, so you have a
mental model of *why* the shape is what it is — not because you have
to manage it.

PJ4's runtime is free to pick its ingest strategy per message:

| Policy | Effect |
|--------|--------|
| **`kEager`** | Invoke the FetchMessageData callable now. Invoke `parseScalars` + `parseObject` now. Copy bytes into the `ObjectStore`. |
| **`kLazyObjectsEagerScalars`** | Invoke the callable now. Invoke `parseScalars` now (columns available immediately). Defer the callable + `parseObject` behind a lazy `ObjectStore` entry, pulled on demand. |
| **`kPureLazy`** | Skip the callable and the parser at push time. Register the callable in the `ObjectStore`; nothing runs until a consumer pulls. |

The selection is done by an `ObjectIngestPolicyResolver` that cascades
`topic > source > kind > default`, configured by the runtime. In PJ4
this will eventually be user-facing — per dataset, per topic, per
kind — but the plugin contract does not change when those controls
land. A plugin written today against the declarative shape will keep
working unmodified when the user starts flipping policies in a future
PJ4 release.

### What this means in practice for the plugin

Even though you do not touch policy code, the existence of lazy modes
constrains how you implement the plugin:

- **Keep the source open and seekable for as long as the host might
  call your FetchMessageData callable.** For a DataLoader, that
  usually means holding the file handle / reader as a `shared_ptr`
  and capturing it inside every closure (and into the `BufferAnchor`
  if the bytes are spans over a mapped or cached chunk). The host may
  invoke your callable seconds, minutes, or hours after `pushMessage`
  returned — and in any order, because the user may scrub through
  time. The specifics depend on the underlying technology (mmap, an
  indexed reader, a chunked decompressor, …), but the contract is the
  same: *if your closure is invoked, it must succeed*.
- **Do not cache decoded data inside the plugin.** The entire
  motivation for lazy ingest is to handle datasets that do not fit in
  memory — a 200 GB MCAP recording of camera streams, a multi-hour
  bag of lidar frames. If the plugin holds onto decoded frames or
  duplicates payload bytes "to be helpful", it defeats the point. Let
  the host's `ObjectStore` be the single owner of materialized data;
  the plugin keeps only what it strictly needs to *fetch* bytes again
  on demand (file offsets, chunk indices, the reader itself).
- **Make the callable idempotent.** The host may invoke it zero, one,
  or many times for the same message — once at eager-time and again
  later when a viewer pulls; multiple times across a session if the
  store evicts and refetches. Returning the same bytes each call
  (modulo a fresh anchor) is fine; doing one-shot work that the
  closure cannot repeat is not.

## End-to-end dispatch

Once the source has called `pushMessage`, the
host follows one of three paths through the resolver, the parser, and
the `ObjectStore`. Streams take only the eager path; files can take
any of the three depending on policy.

```
DataSource          Host (PJ4)                MessageParser         ObjectStore
─────────           ──────────                ─────────────         ───────────
pushMessage(ts,
   fetchMessageData) ──► resolver.resolve(...)
                      ▼
                    kEager ─► fetchMessageData()
                              parser.parseScalars  ─►  schemaHandler.parse_scalars
                              parser.parseObject   ─►  schemaHandler.parse_object
                              pushOwned(bytes)                              ─► [bytes]

                    kLazyObjectsEagerScalars ─► fetchMessageData()
                                                parser.parseScalars  ─►  schemaHandler.parse_scalars
                                                pushLazy(closure)             ─► [FetchMessageData]
                                                                  (pull later)         ─► fetchMessageData + parseObject

                    kPureLazy ─► pushLazy(closure)                              ─► [FetchMessageData]
                                                                  (pull later)         ─► fetchMessageData + parseObject
```

## Authoring checklist

### For a new self-parsing DataSource (Shape A)

1. Subclass `FileSourceBase` and declare
   `kCapabilityDirectIngest` from `extraCapabilities()`.
2. Open the source, walk it, decode in place.
3. For each topic, call `writeHost().ensureTopic(...)` + per-field
   `ensureField(...)`, then `writeHost().appendRecord(topic, ts,
   fields)` per message.
4. Do not advertise channels, do not bind parsers — the parser
   surface stays unused.

### For a new delegating DataSource (Shape B)

1. Subclass `FileSourceBase` (file loader) or `StreamSourceBase`
   (streaming) and declare `kCapabilityDelegatedIngest`.
2. For every channel, build a `ParserBindingRequest` carrying the
   parser encoding (`"protobuf"` / `"ros1msg"` / `"cdr"` / …), the
   type name, and the schema bytes. Resolve a handle through
   `runtimeHost().ensureParserBinding(...)`.
3. **File**: for each message, call `runtimeHost().pushMessage(...)`
   with a FetchMessageData closure that returns a `PayloadView` over
   your buffer (use a `BufferAnchor` so the bytes stay alive past the
   call).
   **Stream**: for each message, call `runtimeHost().pushMessage(...)`
   with a closure that returns the just-received payload (own a copy —
   the receive buffer is reused). The host resolves it eagerly.
4. Do not call the parser, do not consult policy, do not touch the
   `ObjectStore`.

### For a new MessageParser (Shape C)

1. Subclass `MessageParserPluginBase` (or your family's intermediate
   base).
2. Declare a `"encoding"` array in the manifest — the routing key the
   host uses to match `ParserBindingRequest::parser_encoding`.
3. Add a static `catalog()` returning the `CatalogEntry` table. Add a
   `CatalogEntry::kDefault` entry if the plugin should fall back to a
   generic handler for unknown schemas; omit it to reject unmatched
   types at bind time.
4. Implement the `parse_scalars` / `parse_object` member functions
   referenced by the catalog. Prefer spans over the input payload;
   carry the input `BufferAnchor` into the builtin object.
5. In `bindSchema`, call
   `registerSchemaHandler(type_name, makeHandler(catalog().resolve(type_name)))`.

## Reference implementations in this repo

| Plugin | Role | What to read it for |
|--------|------|---------------------|
| `data_load_ulog` | Self-parsing DataSource | Direct ingest pattern: open + decode + `writeHost().appendRecord(...)`. No parser binding. |
| `data_load_csv` | Self-parsing DataSource | Same pattern as ULog with a tiny in-tree CSV decoder. |
| `data_load_parquet` | Self-parsing DataSource | Direct ingest with Arrow/Parquet column-oriented reads. |
| `data_load_mcap` | Delegating DataSource (file) | Deferred FetchMessageData closure capturing the open `McapReader`; reusing decompressed chunks as `BufferAnchor`. |
| `data_stream_zmq` / `data_stream_mqtt` | Delegating DataSource (stream) | Eager `pushMessage` whose closure returns the payload pulled off the socket / broker queue. |
| `parser_ros` | MessageParser | Static catalog with builtin-object handlers (`sensor_msgs/Image` + `CompressedImage` unified into `sdk::Image`, `sensor_msgs/PointCloud2`) and many scalar handlers; default introspection fallback. |
| `parser_protobuf` | MessageParser | Shared across MCAP, ZMQ, MQTT through the encoding string `"protobuf"`. Reads `FileDescriptorSet` from the schema. |
