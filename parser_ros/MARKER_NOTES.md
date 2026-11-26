# parser_ros: visualization_msgs/Marker -> SceneEntities

Design notes for the Marker / MarkerArray -> `PJ::sdk::SceneEntities` conversion
(`ros_marker_handlers.cpp`).

## Canonical target: `kSceneEntities`

`visualization_msgs/Marker` and `MarkerArray` map onto the `sdk::SceneEntities` builtin object, the
MarkerArray equivalent modelled on Foxglove's `SceneUpdate`/`SceneEntity`. One `Marker` becomes one
`SceneEntity` (ADD/MODIFY) or one `SceneEntityDeletion` (DELETE/DELETEALL).

| Marker.type | SceneEntity primitive | Notes |
|---|---|---|
| CUBE / SPHERE / CYLINDER | Cube/Sphere/CylinderPrimitive | `size = scale` |
| LINE_STRIP / LINE_LIST | LinePrimitive | `thickness = scale.x`; pose + points carried as-is |
| TRIANGLE_LIST | TrianglePrimitive | per-vertex colors when `colors.size() == points.size()` |
| TEXT_VIEW_FACING | TextPrimitive | `billboard = true`, `font_size = scale.z` (approximate) |
| ARROW | ArrowPrimitive | dims approximated from scale or the 2-point form; orientation not reconstructed |
| CUBE_LIST / SPHERE_LIST | N Cube/Sphere primitives | glyph at `marker.pose.position + point`; the pose's rotation is not composed |
| MESH_RESOURCE | ModelPrimitive | `url = mesh_resource`, `override_color = !mesh_use_embedded_materials`; inline `mesh_file` bytes are attached with a media type inferred from the filename |
| POINTS | skipped | no instanced point glyph; belongs in `kPointCloud` |
| ARROW_STRIP | skipped | no canonical mapping |

Textures (`texture`, `uv_coordinates`) are consumed and dropped: neither `SceneEntities` nor
Foxglove's schema models textured primitives.

## Statefulness is not resolved in the plugin

ROS Markers are a stateful stream (ADD/DELETE/DELETEALL keyed on `(ns, id)`, plus per-marker
`lifetime`). The parser does **not** accumulate that state and should not: `parseObject` is a pure,
lazy, random-access, idempotent decoder. The 3D widget calls `objectStore.latestAt(topic, T)` ->
`parseObject(thatOneMessage)` on every timeline scrub, out of order and repeatedly, so a `(ns, id)`
accumulator in the parser would be corrupted by scrubbing and re-parsing. State is a function of "all
messages up to the playhead T", a consumer concern (as in Foxglove Studio's `TopicMarkers`, which keeps
a `topic -> ns -> id -> renderable` map and applies lifetime / `frame_locked` at the playhead).

Each decoded message is therefore a **self-contained snapshot**:

- ADD/MODIFY -> a `SceneEntity` whose id encodes `(ns, id)` as a length-prefixed string, so distinct
  pairs never alias.
- DELETE -> `SceneEntityDeletion{kMatchingId, id}`.
- DELETEALL -> `SceneEntityDeletion{kAll}`.

A DELETE marker is a real deletion command, never an empty entity (a renderer could not tell that from
a valid invisible one). Incremental publishers render correctly only in a consumer that accumulates
entities across messages and applies `deletions` and `lifetime`; a publisher that sends the complete
set in each `MarkerArray` works with any consumer.

## Decode is positional CDR plus a schema sniff

The Marker wire layout is identical across ROS 2 humble..rolling. Foxy/galactic and ROS 1 differ only
in the block between `colors[]` and `text`: `texture_resource`, `texture`
(`sensor_msgs/CompressedImage`), `uv_coordinates` and the `mesh_file` field exist in humble+ only.
`bindSchema` sniffs the bound `.msg` (`marker_has_texture_block_` from "uv_coordinates",
`marker_has_mesh_file_` from "mesh_file") and the decoder consumes that variable tail accordingly. This
matters even for shapes that do not use those fields: each Marker in a MarkerArray must be fully
consumed to keep the next one aligned.
