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

`MeshMap` stores one array per channel, as NavMap does, so the layers pass
through in their own type; only the geometry is repacked (`navmap_mesh.hpp`):

- **Geometry:** NavMap's per-axis positions and per-corner indices are
  interleaved into float32 xyz and uint32 triplets.
- **Vertex colour:** `red`, `green`, `blue`, `alpha` uint8 vertex channels when
  the map has vertex colours (`alpha` 255 when it has none).
- **Surfaces:** a categorical uint32 `surface` face channel holding each
  NavCel's surface index, labelled by the surface frames. A map has one
  `frame_id`, the header's: NavMap surfaces share the map frame in practice, so a
  map whose surfaces use different frames is drawn in the header frame.
- **Layers:** one face channel each, named after the layer (an empty name
  becomes `layer`, `surface` becomes `surface_layer`, a repeated one gets a
  numeric suffix), in the layer's own type, with its `unit` and its "unknown"
  value as `info.no_data`. A layer listing only some NavCels marks the others
  with that value: NaN for a float layer; an integer layer without one widens to
  float32 with NaN. When a layer's data array differs from its declared type, the
  array present decides the type. A layer whose data does not match the NavCel
  count is dropped.
- **Links** become `MeshFaceLink`s: `kind` = the NavMap link type, `cost` = its
  length.
- **`geometry_id`** becomes its decimal text; 0 or absent becomes "".

A NavMapLayer message becomes a `MeshMapUpdate` with one face patch for the
layer's channel, in the layer's own type and with the same `ChannelInfo`: dense
(no indices) when it covers every NavCel, sparse with its indices otherwise.

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
claims and the bind guard, the geometry and channels, every layer encoding (type,
unit, unknown value, sparse coverage, a type/array mismatch),
NavMapLayer updates, the scalar summaries, and malformed input: every truncated
prefix of a message (run it under AddressSanitizer to catch an unchecked read),
big-endian payloads and oversized sequence lengths.

```bash
ctest --test-dir build -R navmap_parser
```
