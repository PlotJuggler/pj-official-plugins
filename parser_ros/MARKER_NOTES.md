# parser_ros: visualization_msgs/Marker → SceneEntities

Design notes for the Marker / MarkerArray → `PJ::sdk::SceneEntities` conversion
(`ros_marker_handlers.cpp`). Captures decisions that aren't obvious from the code.

## Canonical target: `kSceneEntities` (not a new type)

`visualization_msgs/Marker` and `MarkerArray` map onto the existing
`sdk::SceneEntities` builtin object — the documented MarkerArray equivalent,
itself a port of Foxglove's `SceneUpdate`/`SceneEntity`. One `Marker` →
one `SceneEntity` (ADD/MODIFY) or one `SceneEntityDeletion` (DELETE/DELETEALL).

| Marker.type | → SceneEntity primitive | notes |
|---|---|---|
| CUBE / SPHERE / CYLINDER | Cube/Sphere/CylinderPrimitive | `size = scale` |
| LINE_STRIP / LINE_LIST | LinePrimitive | `thickness = scale.x`; pose+points carried as-is |
| TRIANGLE_LIST | TrianglePrimitive | per-vertex colors when `colors.size()==points.size()` |
| TEXT_VIEW_FACING | TextPrimitive | `billboard=true`, `font_size = scale.z` (approx) |
| ARROW | ArrowPrimitive | dims approximated from scale or the 2-point form (orientation not reconstructed) |
| CUBE_LIST / SPHERE_LIST | N Cube/Sphere primitives | expanded; glyph at `marker.pose.position + point` (rotation composition deferred) |
| MESH_RESOURCE | ModelPrimitive | `url = mesh_resource`, `override_color = !mesh_use_embedded_materials` |
| POINTS | — skipped | no instanced point glyph; belongs in `kPointCloud` |
| ARROW_STRIP | — skipped | no canonical mapping |

## Statefulness is intentionally NOT resolved in the plugin

ROS Markers are a stateful stream (ADD/DELETE/DELETEALL keyed on `(ns, id)`,
plus per-marker `lifetime`). The parser does **not** accumulate that state, and
**should not**: `parseObject` is a pure, lazy, random-access, idempotent
decoder. The 3D widget calls `objectStore.latestAt(topic, T)` →
`parseObject(thatOneMessage)` on every timeline scrub, out of order and
repeatedly. A `(ns,id)` accumulator living in the parser would be corrupted by
scrubbing and re-parse. Statefulness is a function of "all messages up to the
playhead T" — a *consumer* concern, owned by the (future) 3D scene renderable,
mirroring Foxglove Studio's `TopicMarkers` (which keeps a
`topic → ns → id → renderable` map and applies lifetime / `frame_locked` at the
playhead). Rerun, for contrast, has no semantic marker model at all.

So each decoded message is a **self-contained snapshot**:
- ADD/MODIFY → a `SceneEntity` (identity = `(topic, id)`; we encode `(ns, id)`
  into the entity `id` as a length-prefixed string so distinct pairs never alias).
- DELETE → `SceneEntityDeletion{kMatchingId, id}`.
- DELETEALL → `SceneEntityDeletion{kAll}`.

`deletions[]` and `ModelPrimitive` did not exist in `sdk::SceneEntities` before
this work — they were added in plotjuggler_core (PR #98) so the conversion is
lossless. A DELETE marker is therefore a real deletion command, never an empty
entity (which a renderer couldn't distinguish from a valid invisible one).

## Decode is positional CDR + a schema sniff

The Marker wire layout is byte-identical across all supported ROS 2 distros
(humble → rolling). Only EOL foxy/galactic and ROS 1 differ, and only in the
block between `colors[]` and `text`: `texture_resource`, `texture`
(`sensor_msgs/CompressedImage`), `uv_coordinates`, and the later `mesh_file`
field exist in humble+ but not before. `bindSchema` sniffs the bound `.msg`
definition (`marker_has_texture_block_` ← "uv_coordinates",
`marker_has_mesh_file_` ← "mesh_file") and the decoder consumes that variable
tail accordingly. This matters even for shapes that don't use those fields:
each Marker in a MarkerArray must be fully consumed to keep the next one aligned.

## Deferred / not handled

- **Rendering**: there is no `kSceneEntities` consumer in PJ4 yet
  (`Scene3DDockWidget` rejects it). This is parser-side groundwork; it is
  validated by unit tests only, not end-to-end.
- **Incremental publishers** render correctly only once the renderer accumulates
  entities across messages and applies `deletions` + `lifetime`. Until then, a
  full-array publisher (complete set each message) is the supported pattern.
- **Textures** (`texture`/`uv_coordinates`) are consumed but dropped — neither
  `SceneEntities` nor Foxglove's schema models textured primitives.
- **POINTS / ARROW_STRIP** are skipped; **ARROW** dimensioning and **CUBE_LIST/
  SPHERE_LIST** pose composition are approximate (see table).
