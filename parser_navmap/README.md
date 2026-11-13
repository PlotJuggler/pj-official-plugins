# NavMap Parser

Shows [EasyNavigation NavMap](https://github.com/EasyNavigation/NavMap) maps in
PlotJuggler's 3D scene. A NavMap is a navigable surface stored as a triangle
mesh (each triangle is a *NavCel*), grouped into surfaces (one per floor), with
per-NavCel data layers and links between NavCels that do not touch (stairs,
elevators, doors).

| Schema | Encoding | Object route | Scalar route |
|--------|----------|--------------|--------------|
| `navmap_ros_interfaces/msg/NavMap` | `ros2msg` | `kMeshMap` | summary series |
| `navmap_ros_interfaces/msg/NavMapLayer` | `ros2msg` | `kMeshMapUpdate` | summary series |

The parser only converts. Which layer colours the mesh, the colormap and the
range are chosen in the 3D view, which also pairs a map topic with its updates
topic (`navmap` + `navmap_updates`) and applies the updates over time.

## How it is selected

This is a regular MessageParser that registers a `SchemaHandler` for exactly
these two types. The host's route resolver always prefers an exact-type claim
over a parser that accepts every type of the encoding, so NavMap topics come
here while every other ROS 2 topic stays with `parser_ros`. Both routes are
claimed, so the scalar route plots a summary instead of the generic flattening of
million-element arrays:

- NavMap: `/header/stamp`, `/header/frame_id`, `/num_vertices`, `/num_navcels`,
  `/num_surfaces`, `/num_layers`, and, when the layout has them,
  `/has_vertex_rgba`, `/num_links`, `/geometry_id`;
- NavMapLayer: `/header/stamp`, `/header/frame_id`, `/name`, `/num_values`,
  `/sparse`, `/geometry_id`.

The host also gives every parser a catch-all scalar claim for each encoding in
its manifest and accepts it whenever `bindSchema()` succeeds. `bindSchema()`
therefore rejects every other type; keep it that way.

## Conversion

NavMap stores one array per coordinate and per triangle corner; `MeshMap` wants
interleaved records, so the parser transcodes once (`navmap_mesh.hpp`):

- **Vertices:** `x`, `y`, `z` (float32), plus `red`, `green`, `blue`, `alpha`
  (uint8) when the map has vertex colours (`alpha` 255 when it has none).
- **Faces:** `vertex_indices`, `surface` (the surface index, 0 when the map has
  no surfaces), then one channel per layer, named after it. A name that is empty
  or clashes with `vertex_indices`, `surface` or `index` gets a `_layer` suffix,
  and a repeated name a numeric one.
- **Layer types:** a layer keeps its type unless it has an "unknown" value or
  lists only some NavCels; it then becomes float32 with NaN for "no data",
  because MeshMap has no integer "no data" value. An int16 layer with unit
  `1/1000 log-odds` becomes the float32 occupancy probability. A layer whose data
  does not match the NavCel count is dropped.
- **Links** become `MeshFaceLink`s (`kind` = the NavMap link type).
- **`geometry_id`** becomes its decimal text; 0 or absent becomes "".
- **Frame:** a MeshMap has one frame, the header's. NavMap surfaces share the
  map frame in practice; a map whose surfaces use different frames is drawn in
  the header frame.

A NavMapLayer message becomes a `MeshMapUpdate` with `target = kFaces`: one
record per listed NavCel (every NavCel when `indices` is empty), holding `index`
and the layer channel, encoded as above (a sparse update is not widened: it only
lists what it changes).

## Message layouts

The NavMap messages changed across releases (layers gained metadata, `int16`
data and sparse indices; the map gained `geometry_id` and links). The parser
compiles the `.msg` bundle each topic carries (`navmap_msg_decoder.hpp`) and
decodes by it, so every layout works and future fields are skipped by type. A
topic without a schema is decoded with the current layout.

CDR decoding uses nanocdr, vendored verbatim under `contrib/nanocdr/` (see its
README). nanocdr does not defend against hostile input, so the parser only
reaches it through `navmap_cdr_reader.hpp`, which checks every read against the
remaining payload first and rejects big-endian payloads, which nanocdr would
decode wrongly.

## Host requirement

`min_sdk_required` is 0.41.0, the SDK release that adds `MeshMap` and
`MeshMapUpdate`: an older host cannot decode either object. The SDK floor
checker does not track builtin object types, so this floor is set by hand.

## Tests

`tests/navmap_parser_test.cpp` loads the built `.so` through the host loader and
serializes messages by hand (current and first released layouts). It covers the
claims and the bind guard, the mesh records and channels, every layer encoding,
NavMapLayer updates, the scalar summaries, and malformed input: every truncated
prefix of a message (run it under AddressSanitizer to catch an unchecked read),
big-endian payloads and oversized sequence lengths.

```bash
ctest --test-dir build -R navmap_parser
```
