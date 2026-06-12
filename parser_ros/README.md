# ROS Message Parser

Decodes ROS 1 and ROS 2 (CDR) messages using `rosx_introspection`, with
canonical-object handlers for image, compressed image, and point cloud
types, plus specialized scalar handlers for common sensor types.

Registered for `"ros1msg"`, `"ros2msg"`, and `"omgidl"` schema encodings. CDR is
the ROS 2 wire serialization, not the schema encoding used to select the parser.

## Architecture — declarative schema catalog

`parser_ros` declares a **static catalog** of schemas it knows how to
translate. The catalog
includes a default entry (`CatalogEntry::kDefault`, keyed `"*"`) for
the generic introspection-based fallback, so `bindSchema` is a single
lookup with no branching:

```cpp
const auto& RosMsgParser::catalog() {
  using ObjectType = PJ::sdk::BuiltinObjectType;
  static const std::unordered_map<std::string, CatalogEntry> kMap = {
      // Canonical-object schemas: produce zero-copy sdk::Image /
      // PointCloud, plus header scalars.
      {"sensor_msgs/msg/Image",
          {.object_type   = ObjectType::kImage,
           .parse_scalars = &RosMsgParser::imageScalars,
           .parse_object  = &RosMsgParser::parseImage}},
      {"sensor_msgs/msg/CompressedImage",
          {.object_type   = ObjectType::kImage,
           .parse_scalars = &RosMsgParser::compressedImageScalars,
           .parse_object  = &RosMsgParser::parseCompressedImage}},
      {"sensor_msgs/msg/PointCloud2",
          {.object_type   = ObjectType::kPointCloud,
           .parse_scalars = &RosMsgParser::pointCloud2Scalars,
           .parse_object  = &RosMsgParser::parsePointCloud2}},

      // Scalar-only schemas: column extraction, no canonical object.
      {"sensor_msgs/msg/Imu",
          {.parse_scalars = wrapVoidHandler<&RosMsgParser::handleImu>()}},
      {"sensor_msgs/msg/JointState",
          {.parse_scalars = wrapVoidHandler<&RosMsgParser::handleJointState>()}},
      // ... etc.

      // Default entry — used for any schema not matched above.
      // Drives the generic rosx_introspection walker that flattens
      // nested messages and emits one column per primitive field.
      {CatalogEntry::kDefault,
          {.parse_scalars = &RosMsgParser::introspectionScalars}},
  };
  return kMap;
}

PJ::Status RosMsgParser::bindSchema(std::string_view type_name,
                                     PJ::Span<const uint8_t> schema) {
  base::bindSchema(type_name, schema);
  // Store the schema. PJ4 passes parser_config_json immediately after
  // bindSchema(), so loadConfig({"schema_encoding":"omgidl"}) selects the
  // rosx_introspection schema format before compiling/registering handlers.
  return PJ::Status::ok();
}
```

`catalog().resolve(name)` returns the exact-match entry if present and
falls back to the `"*"` entry otherwise — guaranteed by the catalog
construction. The entries are pure data — member-function pointers, no
`this` capture. Per bound schema, the base class invokes `parse_scalars`
for column extraction and `parse_object` for canonical media bytes.

Delegated sources pass the selected schema encoding through parser config:
`{"schema_encoding":"ros2msg"}` or `{"schema_encoding":"omgidl"}`. This keeps
ROS `.msg` and OMG IDL selection explicit without guessing from type names or
schema text.

## Canonical-object handlers (zero-copy)

Three schemas produce canonical objects whose `data` spans
sit over the original CDR payload, kept alive by the `BufferAnchor`
shipped in the `PayloadView`:

| Schema | `BuiltinObjectType` | Handler |
|--------|-----------------------|---------|
| `sensor_msgs/msg/Image` | `kImage` | `parseImage` — populates `sdk::Image` (width, height, encoding string, `row_step`, `data`) |
| `sensor_msgs/msg/CompressedImage` | `kImage` | `parseCompressedImage` — returns unified `sdk::Image` with `jpeg`, `png`, or `compressedDepth` encoding and detects compressed-depth extras |
| `sensor_msgs/msg/PointCloud2` | `kPointCloud` | `parsePointCloud2` — fills `sdk::PointCloud` with point step, row step, and the raw `data` span |

The canonical object's lifetime is tied to the input payload — the host
copies into the `ObjectStore` only when policy demands materialization.

One schema is promoted **eagerly instead of zero-copy** — the wire carries
polar ranges, so cartesian points must be generated:

| Schema | `BuiltinObjectType` | Handler |
|--------|-----------------------|---------|
| `sensor_msgs/msg/LaserScan` | `kPointCloud` | `parseLaserScan` — projects rays to x/y/z (+`intensity` when present) FLOAT32 points via the shared `pj_laser_scan` projector (cos/sin LUT cached per scanner config); non-finite and out-of-`[range_min, range_max]` rays are dropped, so the cloud is unorganized and dense. The point buffer is newly generated and owned via the cloud's `BufferAnchor`. The scalar route stays on the generic flatten (`angle_*`, `ranges[i]`, … columns). |

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
the host asks. Per-topic / per-source / per-type defaults are configured
through the runtime's `ObjectIngestPolicyResolver`.

## Adding a new schema

1. Add a new entry to `catalog()` in `RosMsgParser.cpp`. Choose a
   `BuiltinObjectType` if the schema maps to one (image, compressed
   image, point cloud); leave it `kNone` for scalar-only schemas.
2. Implement the handler member function(s):
   - `parse_scalars(const ParseContext&, ScalarSink&)` for columns.
   - `parse_object(Timestamp, sdk::PayloadView)` for canonical media bytes
     (if any).
3. Add a focused test under `parser_ros/tests/`.

No enum to extend, no switch to update, no virtual methods to
override — registration is just an entry in the table.

## Timestamp handling

Extracts `header.stamp` (sec + nanosec) as the message timestamp when
`use_embedded_timestamp` is enabled in the parser configuration.
