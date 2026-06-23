# MCAP Raw Ingest Plan

## Problem

Mosaico currently works well for topics that can be materialized as ontology-backed
Arrow tables. The local MCAP upload path drops messages that do not have a
Mosaico ROS adapter, so `toolbox_mosaico` never sees raw ROS2 CDR or protobuf
messages that PlotJuggler could otherwise parse through its message-parser
plugins.

This matters for recordings such as `tomato_1min.mcap`, where the MCAP contains:

| topic | schema | channel encoding | schema encoding | role |
| --- | --- | --- | --- | --- |
| `/lidar/front/rslidar_points` | `sensor_msgs/msg/PointCloud2` | `cdr` | `ros2msg` | 3D point cloud |
| `/tf` | `tf2_msgs/msg/TFMessage` | `cdr` | `ros2msg` | dynamic frame graph |
| `/tf_static` | `tf2_msgs/msg/TFMessage` | `cdr` | `ros2msg` | static frame graph |

For Scene3D compatibility, `/tf` and `/tf_static` must arrive as canonical
`kFrameTransforms` objects, and point clouds must arrive as canonical
`kPointCloud` objects. Treating these topics as scalar Arrow columns is not
enough.

## Existing PlotJuggler Capabilities

`data_load_mcap` already has the right ingest model for local files:

1. Read MCAP `Channel` and `Schema` records.
2. Build a `PJ::ParserBindingRequest` with:
   - `topic_name`
   - `parser_encoding`
   - `type_name`
   - `schema`
   - `parser_config_json`
3. Call `runtimeHost().ensureParserBinding(request)`.
4. Push each raw message with `runtimeHost().pushMessage(handle, ts, fetcher)`.

`parser_ros` already promotes the canonical topics we need:

| type | parser encoding | canonical object |
| --- | --- | --- |
| `sensor_msgs/msg/PointCloud2` | `ros2msg` with `serialization=cdr` | `kPointCloud` |
| `tf2_msgs/msg/TFMessage` | `ros2msg` with `serialization=cdr` | `kFrameTransforms` |
| `geometry_msgs/msg/TransformStamped` | `ros2msg` with `serialization=cdr` | `kFrameTransforms` |

`parser_protobuf` similarly handles protobuf schemas when supplied with the
proper descriptor bytes. Some well-known protobuf schemas may not require schema
bytes, but the raw path should still forward them when Mosaico has them.

## PJ4 Runtime Support

PJ4 commit `4671b70d2ca40140a2699b5d6578ebb41d0e6e7d` added the toolbox-side
parser ingest API needed here. It does not expose `ensureParserBinding()` and
`pushMessage()` directly on `ToolboxRuntimeHostView`; instead it adds a
toolbox-runtime tail-slot pair:

- `create_parser_ingest(data_source_id, out_host, out_error)`
- `release_parser_ingest(data_source_id, out_error)`

The C++ wrapper is:

```cpp
auto ingest = runtimeHost().createParserIngest(ds.id);
auto binding = ingest->ensureParserBinding(request);
auto status = ingest->pushMessage(*binding, timestamp_ns, fetcher);
auto release = runtimeHost().releaseParserIngest(ds.id);
```

The returned `ParserIngestHostView` reuses the same delegated-ingest surface as
DataSource plugins. The host binds parser output to the toolbox-created data
source id, so parser-produced scalars and canonical ObjectStore topics land
under the same dataset as the rest of the Mosaico download.

The remaining PJ-side requirement is dependency alignment: `toolbox_mosaico`
must build and run against a PJ SDK/runtime that includes the commit above. This
PR aligns the plugin checkout with that SDK:

- `SDK_VERSION` is `0.11.0`.
- `extern/plotjuggler_core` is pinned at `v0.11.0`.

So no new PJ4 API design is required, but the plugin repo must be bumped to the
SDK/runtime containing `createParserIngest()` before older checkouts can call
it.

No Mosaico server change is needed for parsing itself. Mosaico only needs to
preserve and expose the raw MCAP channel data and metadata so the PJ client can
delegate decoding to its installed parser plugins.

## Required Mosaico Contract

For every MCAP channel that cannot be represented as an ontology Arrow table,
Mosaico should store one raw-message topic with:

### Topic metadata

Required keys:

| key | value |
| --- | --- |
| `pj.raw.kind` | `mcap_channel` |
| `pj.topic_name` | original MCAP channel topic, e.g. `/tf` |
| `pj.topic_type` | schema name, e.g. `tf2_msgs/msg/TFMessage` |
| `pj.parser_encoding` | parser-facing schema encoding, e.g. `ros2msg`, `omgidl`, `protobuf` |
| `pj.serialization` | message serialization, e.g. `cdr`, `ros1`, `protobuf` |
| `pj.schema_encoding` | original MCAP schema encoding |
| `pj.schema_b64` | base64 schema bytes when available |

Optional keys:

| key | value |
| --- | --- |
| `pj.parser_config_json` | parser configuration override |
| `mcap.channel.message_encoding` | original channel `messageEncoding` |
| `mcap.schema.name` | original schema name |
| `mcap.schema.encoding` | original schema encoding |

### Arrow table shape

Required columns:

| column | type | notes |
| --- | --- | --- |
| `timestamp_ns` | int64/timestamp | host timestamp; log time is safest for MCAP replay |
| `payload` | binary/large_binary/binary_view | original message data bytes |

Optional aliases and fields:

| column | type | notes |
| --- | --- | --- |
| `data` | binary | accepted alias for `payload` |
| `publish_timestamp_ns` | int64/timestamp | useful for diagnostics |
| `sequence` | uint32/int64 | MCAP message sequence |

This mirrors the MCAP model: channel/topic and schema are per-channel metadata,
while each message contributes timestamp plus payload bytes.

## Required `toolbox_mosaico` Change

Once `pj-official-plugins` is aligned with the SDK/runtime containing
`createParserIngest()`:

1. In `FetchWorker`, add a runtime-host provider beside the existing toolbox
   write-host provider.
2. In `MosaicoDialog::setRuntimeHostProvider`, forward the provider to the
   worker, not only to the dialog notification path.
3. In `FetchWorker::pullTopicsAsync`, inspect `TopicInfo::user_metadata` and the
   Arrow schema before `flattenStructColumns()`.
4. If `pj.raw.kind=mcap_channel`, route to a new raw parser path and return.
5. Raw parser path:
   - resolve timestamp column via `detectTsField()`;
   - find `payload` or `data`;
   - decode schema from `pj.schema_b64`;
   - build `parser_config_json` with at least:
     - `schema_encoding`
     - `serialization`
     - `topic_name`
   - call `runtimeHost().createParserIngest(ds.id)` after creating/fetching the
     download data source;
   - bind the parser once per topic;
   - push every Arrow row as a raw parser message;
   - call `runtimeHost().releaseParserIngest(ds.id)` once all raw rows for that
     dataset have been pushed.
6. Call `notifyDataChanged()` after release so parser-produced rows and object
   topics are flushed and visible.
7. Keep the existing image/object and scalar Arrow paths unchanged for
   ontology-backed topics.

Important: raw topics must bypass `flattenStructColumns()` and
`appendArrowStream()`. The raw payload column is not a scalar time series; it is
transport data for the parser.

## Parser Binding Rules

Use the same binding behavior as `data_load_mcap`:

| MCAP channel | parser request |
| --- | --- |
| `messageEncoding=cdr`, `schema.encoding=ros2msg` | `parser_encoding=ros2msg`, `serialization=cdr` |
| `messageEncoding=cdr`, `schema.encoding=omgidl` | `parser_encoding=omgidl`, `serialization=cdr` |
| `messageEncoding=ros1` or `schema.encoding=ros1msg` | `parser_encoding=ros1msg`, `serialization=ros1` |
| `messageEncoding=protobuf` | `parser_encoding=protobuf`, `serialization=protobuf` |

For ROS, normalize names from `pkg/msg/Type` to the parser-compatible
`pkg/Type` only if the parser binding rejects the MCAP spelling. `parser_ros`
already handles the `pkg/msg/Type` form in current code, so the first attempt
should preserve the original schema name.

## Tomato Validation Plan

1. Upload `tomato_1min.mcap` to Mosaico using a raw-preserving uploader.
2. Verify Mosaico lists all three topics:
   - `/lidar/front/rslidar_points`
   - `/tf`
   - `/tf_static`
3. Fetch the topics from `toolbox_mosaico`.
4. Confirm `FetchWorker` routes all three through the raw parser path.
5. Confirm parser bindings:
   - `/lidar/front/rslidar_points` binds to `parser_ros` as
     `sensor_msgs/msg/PointCloud2`.
   - `/tf` and `/tf_static` bind to `parser_ros` as
     `tf2_msgs/msg/TFMessage`.
6. Confirm ObjectStore topics:
   - pointcloud topic metadata has `builtin_object_type=kPointCloud`;
   - TF topics have `builtin_object_type=kFrameTransforms`.
7. Confirm Scene3D:
   - point clouds render at the correct timestamps;
   - `/tf_static` transforms are available from the start of playback;
   - `/tf` transforms update during playback;
   - point clouds resolve against the TF tree.
8. Confirm scalar side effects:
   - parser-generated scalars for TF remain available where applicable;
   - no extra opaque `payload` scalar series is imported.

## Fallback Behavior

If a raw topic is present but the PJ runtime does not expose
`create_parser_ingest`:

1. Do not import the payload column as scalar data.
2. Mark the topic failed with:
   `raw parser ingest requires a PlotJuggler toolbox runtime with parser delegation`.
3. Continue importing other selected topics.

If a parser binding fails:

1. Surface the parser error for that topic.
2. Continue importing other topics.
3. Keep `/tf` and `/tf_static` failures prominent because Scene3D depends on
   them for pointcloud placement.

## Implementation Status

Done in this PR:

1. Bump `pj-official-plugins` to SDK `0.11.0`, which contains
   `createParserIngest()`.
2. Add `toolbox_mosaico` metadata extraction tests for raw MCAP topic metadata.
3. Add the `FetchWorker` raw branch and row-push helper using
   `ParserIngestHostView`.

Still required outside this plugin PR:

1. Add an uploader/server-side raw-preservation path.
2. Run the tomato validation plan against a local Mosaico server.

## Decision

Maximum compatibility requires preserving raw MCAP messages in Mosaico and using
the PJ4 toolbox parser-ingest API added in
`4671b70d2ca40140a2699b5d6578ebb41d0e6e7d`. No further PJ4 API change should be
needed if `toolbox_mosaico` builds against that SDK/runtime. Implementing a
second ROS/protobuf decoder in the toolbox would duplicate parser logic and still
miss future parser plugins.
