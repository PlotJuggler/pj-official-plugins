# ROS Message Parser

Decodes ROS 1 and ROS 2 (CDR) messages with `rosx_introspection`. Common sensor, geometry,
log and grid schemas are also promoted to canonical objects for the image, 3D, 2D and log views.

Registered for the `"ros1msg"`, `"ros2msg"`, `"ros2idl"` and `"omgidl"` schema encodings. CDR is
the ROS 2 wire serialization, not a schema encoding. MCAP `ros2idl` schemas resolve the named root
file and its `.idl` includes from the bundled definitions; unrelated files are ignored.

## Configuration

| Key | Meaning |
|-----|---------|
| `schema_encoding` | `ros1msg`, `ros2msg`, `ros2idl` or `omgidl`. Without it the format is inferred from the schema (scoped `pkg::Type` names and `module` blocks mean IDL). |
| `serialization` | `cdr` (default) or `ros1`. |
| `max_array_size`, array policy | Arrays longer than this (default 500) are clamped or discarded. |
| `use_embedded_timestamp` | Use `header.stamp` as the sample time instead of the receive/log time. A non-positive stamp is ignored. |
| `boolean_strings_to_number` | String fields `"true"`/`"false"` become 1/0. |
| `remove_suffix_from_strings` | String fields such as `"100ms"` become 100 (a leading `{digit + - .}` prefix; `1e3km` yields 1). |

## Scalar output

The generic path walks the schema and emits one column per primitive field, nested messages joined
with `/` and arrays as `name[i]`. Any `geometry_msgs/Quaternion` also gets `roll`/`pitch`/`yaw`
columns. A ROS 2 message that leads with a `std_msgs/Header` gets a combined `/header/stamp`
(seconds) next to the split `sec`/`nanosec` leaves.

Specialized handlers cover `Pose`, `PoseStamped`, `Transform`, `TransformStamped`, `Imu`,
`Odometry`, `JointState`, `DiagnosticArray`, `tf2_msgs/TFMessage`, DataTamer
(`data_tamer_msgs/Schemas` + `Snapshot`), PAL Statistics and TSL. The DataTamer, PAL and TSL
messages are split in two: the names/schema message is remembered and applied to the values/snapshot
message that follows (TSL values arriving first are buffered).

## Canonical objects

The object bytes alias the original payload (zero-copy, kept alive by its anchor) unless noted. An object-bearing schema also
keeps a scalar route so that its metadata stays plottable under any ingest policy.

| Schema | Object | Notes |
|--------|--------|-------|
| `sensor_msgs/Image` | Image | `step` must cover `width * bytes-per-pixel`. Encoding string passed through; only the raw rgb/bgr/mono/`16UC1`/`32FC1`/Bayer encodings are accepted (Bayer is one sample per pixel). |
| `sensor_msgs/CompressedImage` | Image | `jpeg`, `png` or `compressedDepth`. A leading PNG signature on `compressedDepth` means no 12-byte config header (some recorders omit it); a 16UC1 PNG is raw millimetres and takes the depth path. |
| `sensor_msgs/CameraInfo` | CameraInfo | Stops after `P`. |
| `foxglove_msgs/CompressedVideo` | VideoFrame | |
| `sensor_msgs/PointCloud2` | PointCloud | Packed `rgb`/`rgba` colour is repacked to the canonical `rgba` field (a fresh buffer: zero-copy is given up only for colour clouds, via the shared `pj_pointcloud_color`, as in `parser_protobuf`). Separate `red`/`green`/`blue`/`alpha` channels collapse zero-copy. Scalars: header and `/num_points` = `len(data) / point_step` (the points actually present, not `width * height`). |
| `foxglove_msgs/CompressedPointCloud`, `point_cloud_interfaces/CompressedPointCloud2` | CompressedPointCloud | The blob is repackaged, not decoded; the foxglove pose is dropped (place clouds by TF on `frame_id`). Scalars: `/timestamp` + `/frame_id`, or the header only. |
| `sensor_msgs/LaserScan` | PointCloud | Eagerly projected to x/y/z (+`intensity`); non-finite and out-of-`[range_min, range_max]` rays are dropped. Generic scalar flatten. |
| `tf2_msgs/TFMessage`, `geometry_msgs/TransformStamped` | FrameTransforms | Per-transform timestamp is the message time, unless `use_embedded_timestamp` is on or the stamp is 0 (a static transform, kept at 0). |
| `geometry_msgs/PoseArray`, `foxglove_msgs/PosesInFrame`, `geometry_msgs/PoseStamped`, `nav_msgs/Odometry`, `nav_msgs/Path` | PosesInFrame | A `Path` becomes one frame at its own header stamp (per-pose stamps dropped). `PoseStamped` and `Odometry` also keep their per-axis scalars; arrays use the discard policy. |
| `geometry_msgs/PoseWithCovarianceStamped`, `nav2_msgs/ParticleCloud` | PosesInFrame | The covariance and the particle weights are not kept in the object; the covariance stays a scalar, the particle arrays use the discard policy. |
| `geometry_msgs/PolygonStamped` | SceneEntities | One frame-locked entity (id `polygon`) with a closed 2 px line in green; each message replaces the previous one. |
| `nav_msgs/OccupancyGrid`, `map_msgs/OccupancyGridUpdate` | OccupancyGrid(Update) | |
| `nav2_msgs/Costmap`, `nav2_msgs/CostmapUpdate` | OccupancyGrid(Update) | Costs translated with nav2's costmap publisher table: 0 free, 1..252 to 1..98, 253 inscribed 99, 254 lethal 100, 255 unknown -1. The cells are a fresh buffer. From Jazzy on nav2 publishes the full `costmap_raw` only when its size or origin changes and the changed window on `costmap_raw_updates` in between, so a bag needs both topics. |
| `grid_map_msgs/GridMap` | GridMap | Transcoded to a row-major grid (`pj_grid_map`): one `FLOAT32` field per layer; a cell where any `basic_layers` entry is non-finite is NaN in every field. The origin is the map's -x/-y corner. A layout `grid_map_core` would not produce is rejected; capped at 16M cells / 64 layers. |
| `foxglove_msgs/Grid` | GridMap | Zero-copy; `row_count = data.size() / row_stride` (a remainder is rejected). |
| `rcl_interfaces/Log`, `rosgraph_msgs/Log` | Log | Levels map ROS 2 10/20/30/40/50 and ROS 1 1/2/4/8/16; anything else is unknown. File, function, line and ROS 1 `topics[]` are not kept. |
| `foxglove_msgs/Log`, `mavros_msgs/StatusText`, `px4_msgs/LogMessage`, `px4_msgs/MavlinkLog` | Log | `foxglove_msgs/Log` levels 0..5 are kept as is; the others use syslog severities 0..7 (EMERGENCY/ALERT/CRITICAL fatal). PX4 stamps count from boot, so the record time is used. |
| `std_msgs/String` on a `robot_description` topic | RobotDescription | URDF, SDF or MJCF text. |
| `visualization_msgs/Marker`, `MarkerArray` | SceneEntities | See `MARKER_NOTES.md`. |
| `yolo_msgs/DetectionArray` | ImageAnnotations | Third-party message; see `YOLO_NOTES.md`. |

Truncated or corrupt messages (a declared length beyond the payload) are rejected, not decoded.

## Adding a schema

Add an entry to `RosParser::catalog()` in `ros_parser.cpp` with its `BuiltinObjectType` (omit it
for scalar-only schemas), the `parse_scalars` route and, for objects, the `parse_object` member
function; then add a test under `tests/`. An object entry needs a `parse_scalars` too: an
object-only entry aborts the message push on non-`kPureLazy` ingest policies.
