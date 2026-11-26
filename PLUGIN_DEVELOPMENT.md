# Plugin Development Guide

Reference for authors of **DataSource** and **MessageParser** plugins in this collection: the three
plugin shapes, what a parser emits (scalars and builtin objects), how one parser is shared across
sources, and how the host dispatches messages.

## The three plugin shapes

DataSources declare their shape through the capability flag returned from `extraCapabilities()`.
The shapes are not interchangeable; the wrong one fights the runtime.

| Shape | What it does | Identifier in code | Examples |
|-------|--------------|---------------------|----------|
| **Self-parsing DataSource** | Reads *and* decodes its file/stream in one plugin. Writes scalar fields straight into the host via `writeHost().appendRecord(...)`. | `kCapabilityDirectIngest` | `data_load_ulog`, `data_load_csv`, `data_load_parquet` |
| **Delegating DataSource** | Reads the file/stream, advertises each channel's schema, and hands raw bytes off via `pushMessage`. Never inspects message content. | `kCapabilityDelegatedIngest` | `data_load_mcap`, `data_stream_zmq`, `data_stream_mqtt`, `data_stream_foxglove_bridge` |
| **MessageParser** | Has no I/O of its own. Decodes bytes on behalf of a delegating source whenever the host calls. | Declares its `"encoding"` in the plugin manifest | `parser_protobuf`, `parser_ros`, `parser_json`, `parser_data_tamer`, `parser_arrow` |

```cpp
// data_load_csv/csv_source.cpp:
uint64_t extraCapabilities() const override {
  return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
}

// data_load_mcap/mcap_source.cpp:
uint64_t extraCapabilities() const override {
  return PJ::kCapabilityDelegatedIngest | PJ::kCapabilityHasDialog;
}
```

**Bundle vs delegate.** Bundle when the format is self-describing and one decoder library owns it
(ULog with `ulog_cpp`, Parquet with Arrow, CSV with a tiny in-tree parser). Delegate when the
container is polyglot (MCAP carries ROS, protobuf or flatbuffers per channel) or the transport has
no schema and the user picks the encoding (ZMQ and MQTT default to `"json"`, configurable per
topic). Delegating avoids an N x M explosion of source-times-parser plugins: one `parser_ros`
decodes ROS messages from `data_load_mcap`, `data_stream_foxglove_bridge` or any future source.

### Shape A: self-parsing DataSource (direct ingest)

Opens the file, decodes it, and writes named scalar values straight into the host's data engine.
No `ensureParserBinding`, no `pushMessage`, no schema advertisement.

```cpp
// data_load_ulog/ulog_source.cpp (abridged)
auto topic = writeHost().ensureTopic(topic_name);
for (size_t fi = 0; fi < field_names.size(); ++fi) {
  writeHost().ensureField(*topic, field_names[fi], field_types[fi]);
}

for (const auto& raw_sample : sub->rawSamples()) {
  extractFlatValues(raw_sample.data().data(), 0, *sub->format(), values);

  row_fields.clear();
  for (size_t j = 0; j < values.size(); ++j) {
    row_fields.push_back({.name = field_names[j], .value = values[j]});
  }

  writeHost().appendRecord(*topic, PJ::Timestamp{ts_ns},
                           PJ::Span<const PJ::sdk::NamedFieldValue>(row_fields));
}
```

### Shape B: delegating DataSource

Enumerates channels, advertises each channel's schema and encoding, and emits messages as
`(binding_handle, timestamp_ns, payload)`. The host routes the bytes to a MessageParser. The source
announces every message exactly once; the host decides whether to read and store it now, read only
the cheap parts, or go back to the file only when a viewer asks, so a multi-gigabyte recording
does not have to fit in memory at load time.

| Sub-shape | Base class | Examples |
|-----------|------------|----------|
| **DataLoader** | `FileSourceBase` | `data_load_mcap` |
| **DataStream** | `StreamSourceBase` | `data_stream_zmq`, `data_stream_mqtt`, `data_stream_foxglove_bridge`, `data_stream_pj_bridge` |

A delegating source does **not**: decode the bytes, know about widgets, write to the `ObjectStore`
or `DataEngine`, or consult `ObjectIngestPolicy` / branch on eager vs lazy. One `pushMessage` per
message, regardless.

### Shape C: MessageParser

A schema-bound decoder: given a schema name and a payload it produces scalar columns (always) and,
when the schema maps to a media type, a builtin object (optionally). It owns no I/O and does not
decide *when* to parse; it answers `parse_scalars` / `parse_object` when the host calls. It binds
by schema, not by topic (two channels with the same schema share one bound handler), and does not
know which transport the bytes came from.

## What a MessageParser emits: scalars and builtin objects

The primary product is **scalar columns**: every `IMU.angular_velocity.x`, `JointState.position[3]`
becomes a `(timestamp, value)` stream for plots, tables, transforms and Lua scripts. Most parsers
(`parser_json`, `parser_protobuf`, `parser_data_tamer`) walk the message, name every primitive and
emit one column per leaf. `parser_arrow` is batch-shaped and hands the stream to the host through
`appendArrowStream(...)`.

Non-scalar messages (camera frames, point clouds) are additionally emitted as **builtin objects**:
media payloads that the image and point-cloud viewers decode. One message can emit both: scalar
metadata (image `width` / `height` / `frame_id` / `encoding`, point-cloud `point_step` /
`row_step`) plus the object carrying the raw bytes. Because such payloads are large in aggregate,
the host typically loads them **on demand** and keeps the FetchMessageData callable (below)
associated with each object.

Every bound schema is described by a `CatalogEntry` with two output slots:

| Slot | Output channel | Consumer |
|------|----------------|----------|
| `parse_scalars` | Columns through the `ScalarSink` | Plotter, tables, transforms, Lua |
| `parse_object` (optional) | A `BuiltinObject` (`std::any`) | Image / depth / point-cloud / annotation viewers |

## Builtin objects

A builtin object is a type-erased value defined in `pj_base/builtin/BuiltinObject.hpp`, and the
only shape a parser may hand to the visualizers:

```cpp
namespace PJ::sdk {
using BuiltinObject = std::any;
[[nodiscard]] BuiltinObjectType typeOf(const BuiltinObject& obj) noexcept;
}  // namespace PJ::sdk
```

| Type | Purpose |
|------|---------|
| `sdk::Image` | Image: `width`, `height`, `std::string encoding` (`"rgb8"`, `"bgr8"`, `"mono8"`, `"jpeg"`, `"png"`, `"compressedDepth"`, ...), `row_step`, `Span<const uint8_t> data`, `BufferAnchor`. The encoding string distinguishes raw from compressed. |
| `sdk::DepthImage` | Depth image: pixels + camera intrinsics (`K` 3x3, `D` distortion, `distortion_model` string). `R` and `P` are derived via free helpers in `depth_image_utils.hpp`. |
| `sdk::PointCloud` | Point-cloud frame: `width`, `height`, `point_step`, `row_step`, field descriptors, raw `Span<const uint8_t> data`, `BufferAnchor`. |
| `sdk::ImageAnnotations` | 2D vector overlays (points, lines, circles, text). Owns its data (`std::vector` members), no anchor. |
| `sdk::FrameTransforms` | Named 3D frame relationships. |

`std::any` (not `std::variant`) lets new builtin types be added without changing `BuiltinObject`:
older plugins keep producing the types they know, and an older host sees
`BuiltinObjectType::kNone` from `typeOf` for unknown types. The catalog's declared `object_type`
lets the host pick an ingest policy before the bytes are touched.

Consumers recover the concrete type with `std::any_cast`:

```cpp
auto obj = parser->parseObject(timestamp_ns, payload);
if (obj && PJ::sdk::typeOf(*obj) == PJ::sdk::BuiltinObjectType::kImage) {
  const auto* img = std::any_cast<PJ::sdk::Image>(&*obj);
  // ... use img->encoding, img->data, img->anchor, ...
}
```

The buffer-backed types are zero-copy: `Span<const uint8_t>` plus a `BufferAnchor` let the parser
return spans over the original payload; the host copies into the `ObjectStore` only when retention
policy demands it. The viewer never learns the wire format (ROS CDR, Foxglove, a video frame all
land as the same `sdk::Image`).

### `PayloadView` and `BufferAnchor`

```cpp
namespace PJ::sdk {
struct PayloadView {
  Span<const uint8_t> bytes;    // the message payload
  BufferAnchor       anchor;    // keeps `bytes` alive
};

using BufferAnchor = std::shared_ptr<const void>;
}
```

`BufferAnchor` is an opaque ownership token (any `shared_ptr<T>`: a vector, a reader holding the
chunk, a `shared_ptr<void>` with a deleter). The host and downstream consumers keep it alive for as
long as they reference spans built from it.

## Delegating DataSources: `pushMessage`

After `ensureParserBinding(...)`, every message is announced with
`pushMessage(handle, ts, fetchMessageData)`. Files pass a lazy closure; streams pass a closure over
the just-received bytes.

### File source

The DataLoader delivers a **callable that produces bytes when invoked**, one per message:

```cpp
// data_load_mcap/mcap_source.cpp (abridged)
auto push_status = runtimeHost().pushMessage(
    binding_it->second, timestamp_ns,
    [reader = reader_keeper_, off = msg_view.messageOffset,
     ch = msg_view.channel->id, lt = msg_view.message.logTime]() {
      return readMessageBytesAt(reader, off, ch, lt);
    });
```

The closure may return:

- `PJ::sdk::PayloadView { bytes, anchor }` (preferred): zero-copy span over a buffer kept alive via
  `BufferAnchor` (a `shared_ptr` to the source's chunk/page, or to the reader itself).
- `std::vector<uint8_t>`: the SDK template heap-allocates it and uses it as its own anchor.

The C ABI counterpart is `PJ_message_data_fetcher_t { ctx, fetchMessageData, release }` in
`pj_base/data_source_protocol.h`; the host releases the context exactly once. **`fetchMessageData`
MUST be thread-safe**: the host may call it from the ingest thread (`kEager`) or from consumer
threads (lazy pulls).

### Stream source

The receive buffer is overwritten by the next frame, so the closure owns a copy. There is no past
to seek, so the host resolves it eagerly (`kEager` is the only policy that applies to streams).

```cpp
// data_stream_zmq/zmq_source.cpp (abridged)
auto status = runtimeHost().pushMessage(
    it->second, PJ::Timestamp{timestamp_ns},
    [bytes = std::vector<uint8_t>(payload_data, payload_data + payload_size)]() { return bytes; });
```

The source never invokes the parser or pushes to the `ObjectStore`; the host does both.

## MessageParser plugins: declarative `SchemaHandler` catalog

A parser does not override `parse()`. It declares a table of handlers, one per schema type name it
translates, plus an optional default entry (`CatalogEntry::kDefault`, keyed `"*"`) used for every
unmatched schema. Without a default, `resolve()` reports an error and the schema is rejected at
`bindSchema` time.

```cpp
// A static, pure-data catalog: member-function pointers, no `this` capture.
const auto& MyParser::catalog() {
  using ObjectType = PJ::sdk::BuiltinObjectType;
  static const std::unordered_map<std::string, CatalogEntry> kMap = {
      // Builtin-object schema: parse_object builds the object, parse_scalars emits small metadata.
      {"my_pkg/MyImage",
          {.object_type   = ObjectType::kImage,
           .parse_scalars = &MyParser::imageScalars,
           .parse_object  = &MyParser::parseImage}},

      // Scalar-only schema.
      {"my_pkg/MyTelemetry",
          {.parse_scalars = &MyParser::telemetryScalars}},

      // Generic fallback for every other schema (optional).
      {CatalogEntry::kDefault,
          {.parse_scalars = &MyParser::genericScalars}},
  };
  return kMap;
}

PJ::Status MyParser::bindSchema(std::string_view type_name,
                                 PJ::Span<const uint8_t> schema) {
  base::bindSchema(type_name, schema);
  // ... parser-specific setup ...

  registerSchemaHandler(type_name,
                        makeHandler(catalog().resolve(type_name)));
  return PJ::Status::ok();
}
```

`CatalogEntry` fields:

- `object_type`: `BuiltinObjectType::kImage` / `kDepthImage` / `kPointCloud` / `kImageAnnotations` /
  `kFrameTransforms` / `kNone`. Lets the host pick the ingest policy before the bytes are touched.
- `parse_scalars`: walks the payload and emits columns through the `ScalarSink`.
- `parse_object`: builds the builtin-object value, with spans over the input buffer where possible.

The SDK base class implements `classifySchema`, `parseScalars` and `parseObject` as table lookups;
adding a schema is a new catalog entry plus its member function.

Reference implementation: `parser_ros` (builtin-object handlers for `sensor_msgs/Image`,
`CompressedImage` and `PointCloud2`, plus scalar handlers for `Imu`, `JointState`, `Pose`, `Twist`,
`TF2`, `DiagnosticArray`, ...).

## Worked example: one parser, three sources

`parser_protobuf` is bound from an MCAP file, a ZMQ socket and an MQTT broker. None of the sources
includes a protobuf header and the parser includes nothing from them.

### Routing

Every MessageParser declares the encodings it handles in its manifest:

```json
{
  "id": "protobuf-parser",
  "name": "Protobuf Parser",
  "category": "message_parser",
  "encoding": ["protobuf"]
}
```

When a source calls `ensureParserBinding(...)` with `parser_encoding = "protobuf"`, the host binds
the registered parser whose manifest claims that encoding. The encoding string is the only routing
key; sources never name a parser class.

### The binding call, three ways

**MCAP: schema and encoding come from the file.**

```cpp
// data_load_mcap/mcap_source.cpp (abridged)
PJ::ParserBindingRequest request{
    .topic_name = channel_ptr->topic,
    .parser_encoding = encoding,          // e.g. "protobuf" - from MCAP channel
    .type_name = schema->name,            // e.g. "my_pkg.MyMessage" - from MCAP schema
    .schema = schema_bytes,               // FileDescriptorSet bytes - from MCAP schema
    .parser_config_json = parser_config_str,
};
auto handle = runtimeHost().ensureParserBinding(request);
```

**ZMQ and MQTT: the user picks the encoding, there is no schema.** The dialog asks for an encoding
(default `"json"`) and the binding goes out with an empty schema. The parser must be
pre-configured: for protobuf, the operator pastes a compiled `FileDescriptorSet` into the parser's
own dialog before the stream starts.

```cpp
auto binding = runtimeHost().ensureParserBinding({
    .topic_name = topic_name,
    .parser_encoding = default_encoding_,   // e.g. "protobuf" - from user dialog
    .type_name = {},                         // stream doesn't know
    .schema = {},                            // pre-configured on the parser
    .parser_config_json = {},
});
```

Swapping `"protobuf"` for `"ros1msg"` / `"cdr"` reaches `parser_ros`; `"json"` reaches `parser_json`.
A new source needs no parser changes and vice versa.

## How the host uses these declarations

Plugins write no policy code. The host picks an ingest strategy per message:

| Policy | Effect |
|--------|--------|
| **`kEager`** | Invoke the FetchMessageData callable now. Invoke `parseScalars` + `parseObject` now. Copy bytes into the `ObjectStore`. |
| **`kLazyObjectsEagerScalars`** | Invoke the callable now. Invoke `parseScalars` now (columns available immediately). Defer the callable + `parseObject` behind a lazy `ObjectStore` entry, pulled on demand. |
| **`kPureLazy`** | Skip the callable and the parser at push time. Register the callable in the `ObjectStore`; nothing runs until a consumer pulls. |

An `ObjectIngestPolicyResolver` selects the policy, cascading `topic > source > kind > default`.

### Constraints on the plugin

- **Keep the source open and seekable as long as the host might call your callable.** For a
  DataLoader, hold the file handle/reader as a `shared_ptr` captured by every closure (and by the
  `BufferAnchor` if the bytes are spans over a mapped or cached chunk). The host may call it
  seconds or hours after `pushMessage` returned, in any order (the user scrubs through time). If
  your closure is invoked, it must succeed.
- **Do not cache decoded data or duplicate payload bytes in the plugin.** Lazy ingest exists for
  datasets that do not fit in memory; the host's `ObjectStore` is the single owner of materialized
  data. Keep only what is needed to fetch the bytes again (offsets, chunk indices, the reader).
- **Make the callable idempotent.** It may run zero, one, or many times for the same message
  (eagerly, again when a viewer pulls, again after the store evicts and refetches). Returning the
  same bytes each call (modulo a fresh anchor) is fine; one-shot work is not.

## End-to-end dispatch

After `pushMessage`, the host follows one of three paths. Streams take only the eager path.

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

**Self-parsing DataSource (Shape A)**

1. Subclass `FileSourceBase` and return `kCapabilityDirectIngest` from `extraCapabilities()`.
2. Per topic: `writeHost().ensureTopic(...)` + per-field `ensureField(...)`, then
   `writeHost().appendRecord(topic, ts, fields)` per message.
3. Do not advertise channels or bind parsers.

**Delegating DataSource (Shape B)**

1. Subclass `FileSourceBase` or `StreamSourceBase` and return `kCapabilityDelegatedIngest`.
2. Per channel, build a `ParserBindingRequest` (parser encoding, type name, schema bytes) and
   resolve a handle with `runtimeHost().ensureParserBinding(...)`.
3. Per message, call `runtimeHost().pushMessage(...)`. File: closure returning a `PayloadView`
   with a `BufferAnchor`. Stream: closure returning an owned copy of the received payload.
4. Do not call the parser, consult policy, or touch the `ObjectStore`.

**MessageParser (Shape C)**

1. Subclass `MessageParserPluginBase` (or your family's intermediate base).
2. Declare a `"encoding"` array in the manifest; it must match
   `ParserBindingRequest::parser_encoding`.
3. Add a static `catalog()`; include `CatalogEntry::kDefault` only if unknown schemas should fall
   back to a generic handler instead of being rejected at bind time.
4. Implement the `parse_scalars` / `parse_object` member functions. Prefer spans over the input
   payload and carry the input `BufferAnchor` into the builtin object.
5. In `bindSchema`, call
   `registerSchemaHandler(type_name, makeHandler(catalog().resolve(type_name)))`.

## Reference implementations

| Plugin | Role | What to read it for |
|--------|------|---------------------|
| `data_load_ulog` | Self-parsing DataSource | Direct ingest: open + decode + `writeHost().appendRecord(...)`. |
| `data_load_csv` | Self-parsing DataSource | Same pattern with a tiny in-tree CSV decoder. |
| `data_load_parquet` | Self-parsing DataSource | Direct ingest with Arrow/Parquet column-oriented reads. |
| `data_load_mcap` | Delegating DataSource (file) | Deferred FetchMessageData closure capturing the open `McapReader`; reusing decompressed chunks as `BufferAnchor`. |
| `data_stream_zmq` / `data_stream_mqtt` | Delegating DataSource (stream) | Eager `pushMessage` whose closure returns the payload pulled off the socket / broker queue. |
| `parser_ros` | MessageParser | Static catalog with builtin-object handlers and many scalar handlers; default introspection fallback. |
| `parser_protobuf` | MessageParser | Shared across MCAP, ZMQ, MQTT through the encoding string `"protobuf"`. Reads `FileDescriptorSet` from the schema. |
