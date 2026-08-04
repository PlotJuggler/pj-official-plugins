# Protocol Buffers Message Parser

Decodes Protocol Buffers messages using dynamic reflection from
FileDescriptorSet schemas.

## Supported Types

- All scalar types: float, double, int32, int64, uint32, uint64, bool
- Nested messages (recursive flattening with `/` separator)
- Repeated fields with bracket notation (`field[0]`, `field[1]`)
- Map fields with key-based paths
- Pre-registration of fields at schema bind time for performance

## Encoding

Registered as parser for `"protobuf"` encoding.

## Schema

Expects a `FileDescriptorSet` binary blob passed via `bindSchema()`.
The parser builds a `DescriptorPool` and uses `DynamicMessageFactory`
for reflection-based decoding.

## Canonical Scene Objects

Some well-known schemas are recognized by name and promoted to builtin scene
objects (instead of flattened scalars), so PlotJuggler can render them:

| Schema | Builtin object |
|--------|----------------|
| `PJ.VideoFrame` / `foxglove.CompressedVideo` | `kVideoFrame` (scene2D) |
| `PJ.PosesInFrame` / `foxglove.PosesInFrame` | `kPosesInFrame` (3D poses) |
| `foxglove.PointCloud` | `kPointCloud` (3D point cloud) |
| `foxglove.LaserScan` | `kPointCloud` (3D point cloud, eagerly projected) |

These bypass the descriptor pool: their wire layout is known, so they are
decoded directly and **zero-copy** (the packed-point / bitstream span aliases
the payload). A slim metadata row is still emitted as plottable scalars: the
embedded `timestamp` (seconds) and `frame_id` that every promoted schema
carries, plus a bounded count or size (e.g. `point_count`, `num_poses`,
`data_size`). The `timestamp` series is always emitted — the row itself is
filed under the host/log clock unless `use_embedded_timestamp` is on, so
without it the message's own stamp would be unplottable.

`foxglove.LaserScan` is the exception to zero-copy: the wire carries polar
ranges, so the handler eagerly projects rays into cartesian x/y/z
(+`intensity`) FLOAT32 points through the shared `pj_laser_scan` projector
(cos/sin LUT cached per scanner config — recomputed only when
`(ray_count, start_angle, angle_increment)` changes). Non-finite ranges are
dropped (Foxglove carries no range bounds), and the generated point buffer is
owned via the cloud's `BufferAnchor`. The same non-identity-`pose` drop policy
as `foxglove.PointCloud` applies. Its scalar route never projects: a header-only
walk emits `timestamp`, `frame_id`, `start_angle`, `end_angle` and
`num_ranges`.

`PJ.PosesInFrame` and `foxglove.PosesInFrame` are wire-identical (the SDK proto
mirrors Foxglove field-for-field), so both names bind to the same SDK codec
(`deserializePosesInFrame`) — no plugin-local decoder, mirroring the
`VideoFrame` fast path. The message is small (scalars + a `frame_id`), so the
decode is owning rather than zero-copy; the scalar route emits `num_poses`.

`foxglove.PointCloud` mirrors how the ROS parser handles
`sensor_msgs/PointCloud2`: geometry is kept in `frame_id` and resolved through
the TF tree. Foxglove's inline `pose` field has no slot in the canonical
`sdk::PointCloud`, so a **non-identity `pose` is dropped** (a one-time warning is
logged). For correct multi-sensor placement, provide the matching
`foxglove.FrameTransform` (`/tf`) stream.

Per-point colour is normalized to the canonical packed field via the shared
`pj_pointcloud_color` helper (`common/pointcloud_color`): foxglove's separate
`red`/`green`/`blue`/`alpha` `uint8` channels collapse into a single
`PointField{ name:"rgba", datatype:kUint32 }` whose 4 bytes are R, G, B, A in
increasing-address order. The bytes are already in that order, so the collapse is a
**pure metadata rewrite that keeps the zero-copy span**. The host then renders one
per-point colour (its "RGB" colour mode) instead of treating each channel as a
separate colormap source. `parser_ros` produces the **same** canonical `rgba` field
(repacking a PCL `0x00RRGGBB` packed colour), so the host has one colour rule.

## Known Limitations

- Embedded timestamp extraction not implemented (original auto-detects
  "timestamp" double field)
- Enum values stored as int32 (original stored as human-readable strings)
- String fields skipped entirely (original included strings < 100 bytes)
- No array size clamping/discard policy
- No interactive dialog for .proto file loading
