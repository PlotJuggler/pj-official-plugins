# ROS Message Parser

Decodes ROS 1 and ROS 2 (CDR) messages using `rosx_introspection`, with
canonical-object handlers for image, compressed image, and point cloud
types, plus specialized scalar handlers for common sensor types.

Registered for `"ros1msg"`, `"ros2msg"`, and `"cdr"` schema encodings.

## Architecture — declarative schema catalog

`parser_ros` does not implement a `parse()` function. Instead it declares
a **static catalog** of schemas it knows how to translate, and lets the
SDK base class dispatch per message:

```cpp
const auto& RosMsgParser::catalog() {
  using Kind = PJ::sdk::CanonicalObjectKind;
  static const std::unordered_map<std::string, CatalogEntry> kMap = {
      // Canonical-object schemas: produce zero-copy sdk::Image /
      // CompressedImage / PointCloud, plus header scalars.
      {"sensor_msgs/msg/Image",
          {.object_kind   = Kind::kImage,
           .parse_scalars = &RosMsgParser::imageScalars,
           .parse_object  = &RosMsgParser::parseImage}},
      {"sensor_msgs/msg/CompressedImage",
          {.object_kind   = Kind::kCompressedImage,
           .parse_scalars = &RosMsgParser::compressedImageScalars,
           .parse_object  = &RosMsgParser::parseCompressedImage}},
      {"sensor_msgs/msg/PointCloud2",
          {.object_kind   = Kind::kPointCloud,
           .parse_scalars = &RosMsgParser::pointCloud2Scalars,
           .parse_object  = &RosMsgParser::parsePointCloud2}},

      // Scalar-only schemas: column extraction, no canonical object.
      {"sensor_msgs/msg/Imu",
          {.parse_scalars = wrapVoidHandler<&RosMsgParser::handleImu>()}},
      {"sensor_msgs/msg/JointState",
          {.parse_scalars = wrapVoidHandler<&RosMsgParser::handleJointState>()}},
      // ... etc.
  };
  return kMap;
}

PJ::Status RosMsgParser::bindSchema(std::string_view type_name,
                                     PJ::Span<const uint8_t> schema) {
  base::bindSchema(type_name, schema);
  // ... rosx_introspection setup ...

  auto it = catalog().find(canonical_name(type_name));
  auto handler = (it != catalog().end())
      ? makeHandler(it->second)
      : makeDefaultIntrospectionHandler();
  registerSchemaHandler(type_name, std::move(handler));
  return PJ::Status::ok();
}
```

The entries are pure data — member-function pointers, no `this` capture.
Per bound schema, the base class invokes `parse_scalars` for column
extraction and `parse_object` for canonical media bytes.

## Canonical-object handlers (zero-copy)

Three schemas produce canonical objects whose `bytes` / `pixels` spans
sit over the original CDR payload, kept alive by the `BufferAnchor`
shipped in the `PayloadView`:

| Schema | `CanonicalObjectKind` | Handler |
|--------|-----------------------|---------|
| `sensor_msgs/msg/Image` | `kImage` | `parseImage` — populates `sdk::Image` (width, height, encoding → `PixelFormat`, `row_step`, `pixels`) |
| `sensor_msgs/msg/CompressedImage` | `kCompressedImage` | `parseCompressedImage` — strips the `format` string + CDR alignment, returns JPEG/PNG bytes and detects `compressedDepth` extras |
| `sensor_msgs/msg/PointCloud2` | `kPointCloud` | `parsePointCloud2` — fills `sdk::PointCloud` with point step, row step, and the raw `data` span |

The canonical object's lifetime is tied to the input payload — the host
copies into the `ObjectStore` only when policy demands materialization.

## Scalar handlers

Specialized handlers (registered through `wrapVoidHandler<>`) extract
columns from common sensor messages:

- **Quaternion fields** — auto-detected anywhere in the schema, adds
  roll/pitch/yaw columns.
- **JointState** — per-joint `position` / `velocity` / `effort` series.
- **DiagnosticArray** — key/value extraction from each status entry.
- **TF2** — frame hierarchy as position + rotation columns per frame.
- **Imu**, **NavSatFix**, **Pose**, **Twist**, **PoseStamped**, … —
  one-shot scalar extractors.
- **DataTamer**, **PAL Statistics**, **TSL** — multi-message buffering
  parsers that pair schema/snapshot or definition/value pairs.

The generic fallback walks the introspection tree and emits one column
per primitive field, with nested messages flattened via `/` and arrays
expanded with bracket notation (up to a configurable element cap).

## How the host uses these handlers

The host (PJ4's runtime) selects how to call into the parser per message:

| Ingest policy | Effect |
|---------------|--------|
| `kEager` | `parse_scalars` + `parse_object` invoked immediately; canonical bytes copied into the `ObjectStore`. |
| `kLazyObjectsEagerScalars` | `parse_scalars` invoked now (columns become available immediately); `parse_object` deferred behind a fetcher closure pulled on demand. |
| `kPureLazy` | Neither invoked at push time; the fetcher closure sits in the `ObjectStore` until something pulls. |

The parser does not know which mode is active — it answers honestly when
the host asks. Per-topic / per-source / per-kind defaults are configured
through the runtime's `ObjectIngestPolicyResolver`.

## Adding a new schema

1. Add a new entry to `catalog()` in `RosMsgParser.cpp`. Choose a
   `CanonicalObjectKind` if the schema maps to one (image, compressed
   image, point cloud); leave it `kNone` for scalar-only schemas.
2. Implement the handler member function(s):
   - `parse_scalars(const ParseContext&, ScalarSink&)` for columns.
   - `parse_object(int64_t pts, sdk::PayloadView, CanonicalSink&)` for
     canonical media bytes (if any).
3. Add a focused test under `parser_ros/tests/`.

No enum to extend, no switch to update, no virtual override surface to
maintain — registration is just an entry in the table.

## Timestamp handling

Extracts `header.stamp` (sec + nanosec) as the message timestamp when
`use_embedded_timestamp` is enabled in the parser configuration.
