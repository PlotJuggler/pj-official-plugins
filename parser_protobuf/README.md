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
| `foxglove.PointCloud` | `kPointCloud` (3D point cloud) |
| `foxglove.LaserScan` | `kPointCloud` (3D point cloud, eagerly projected) |

These bypass the descriptor pool: their wire layout is known, so they are
decoded directly and **zero-copy** (the packed-point / bitstream span aliases
the payload). A slim metadata row (e.g. `frame_id`, `point_count`,
`point_step`, `num_fields`) is still emitted as plottable scalars.

`foxglove.LaserScan` is the exception to zero-copy: the wire carries polar
ranges, so the handler eagerly projects rays into cartesian x/y/z
(+`intensity`) FLOAT32 points through the shared `pj_laser_scan` projector
(cos/sin LUT cached per scanner config — recomputed only when
`(ray_count, start_angle, angle_increment)` changes). Non-finite ranges are
dropped (Foxglove carries no range bounds), and the generated point buffer is
owned via the cloud's `BufferAnchor`. The same non-identity-`pose` drop policy
as `foxglove.PointCloud` applies. Its scalar route never projects: a header-only
walk emits `frame_id`, `start_angle`, `end_angle` and `num_ranges`.

`foxglove.PointCloud` mirrors how the ROS parser handles
`sensor_msgs/PointCloud2`: geometry is kept in `frame_id` and resolved through
the TF tree. Foxglove's inline `pose` field has no slot in the canonical
`sdk::PointCloud`, so a **non-identity `pose` is dropped** (a one-time warning is
logged). For correct multi-sensor placement, provide the matching
`foxglove.FrameTransform` (`/tf`) stream.

## Known Limitations

- Embedded timestamp extraction not implemented (original auto-detects
  "timestamp" double field)
- Enum values stored as int32 (original stored as human-readable strings)
- String fields skipped entirely (original included strings < 100 bytes)
- No array size clamping/discard policy
- No interactive dialog for .proto file loading
