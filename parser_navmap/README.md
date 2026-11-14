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
these two types and claims both routes, so the scalar route plots a summary
instead of the generic flattening of million-element arrays:

- NavMap: `/header/stamp`, `/header/frame_id`, `/num_vertices`, `/num_navcels`,
  `/num_surfaces`, `/num_layers`, `/dropped_layers`, `/dropped_links` (0 for a
  clean map), and, when the layout has them, `/has_vertex_rgba`, `/num_links`,
  `/geometry_id`;
- NavMapLayer: `/header/stamp`, `/header/frame_id`, `/name`, `/num_values`,
  `/sparse`, `/geometry_id`.

**Known limitation:** `parser_ros` also makes an exact claim for every type it
can bind, and the host breaks ties by plugin provenance first. When
`parser_ros` is bundled with PlotJuggler and this plugin is installed from the
marketplace or a plugin directory, `parser_ros` takes the scalar route of
NavMap topics (its generic flattening, without these summaries). The object
route, which the 3D view uses, is unaffected: only this parser claims it.

The host also gives every parser a catch-all scalar claim for each encoding in
its manifest and accepts it whenever `bindSchema()` succeeds. `bindSchema()`
therefore rejects every other type; keep it that way.

## Timestamps

Records and objects are filed under the host's message time (receive or log
time) by default. When the data source sets `use_embedded_timestamp` in the
parser config, as the ROS parser honours it, a message whose header stamp is
positive is filed under that stamp on both routes: the scalar row, the object
record and the object's `timestamp_ns`. A zero or negative stamp keeps the
message time.

## Conversion

`MeshMap` stores one array per channel, as NavMap does, so the layers pass
through in their own type; only the geometry is
repacked (`navmap_mesh.hpp`):

- **Geometry:** NavMap's per-axis positions and per-corner indices are
  interleaved into float32 xyz and uint32 triplets.
- **Vertex colour:** `red`, `green`, `blue`, `alpha` uint8 vertex channels when
  the map has vertex colours (`alpha` 255 when it has none).
- **Surfaces:** a categorical uint32 `surface` face channel holding each
  NavCel's surface index, labelled by the surface frames. A NavCel that no
  surface lists holds 4294967295 (the uint32 maximum), declared as the
  channel's `info.no_data`; when every NavCel is listed the channel has no
  `no_data`. A map has one `frame_id`, the header's (the first surface's when
  the header's is empty, as upstream does): NavMap surfaces share the map frame
  in practice, so a map whose surfaces use different frames is drawn in that
  one frame.
- **Layers:** one face channel each, named exactly after the layer, in the type
  of the array it carries (the type tag only picks which array to read), with
  its `unit` and its "unknown" value as `info.no_data` when that type can hold
  it (an integer channel cannot hold, say, -1 in uint8, and then declares no
  `no_data`). A layer whose name is
  empty, repeats an earlier layer of the map, or is reserved (`surface`, and
  the MeshMap conventions `red`, `green`, `blue`, `alpha`, `normal`, `uv`) is
  dropped, not renamed: the name then depends only on the layer, so a map and
  its updates always address the same channel, in the same type. A layer
  listing only some NavCels marks the others with its no-data value, or NaN in
  a float channel without one; an integer layer without a no-data value it can
  hold has nothing valid to mark them with and is dropped (an update never
  fills, so it is never dropped for this). A layer whose data does not match
  the NavCel count, lists a NavCel out of range, or holds 64-bit integers is
  dropped. So is a sparse layer that would take the memory the map's sparse
  layers expand to (NavCel count times element size each, 1 GiB in all) over
  that limit: a small message cannot demand gigabytes. Layers are kept in
  order until the limit is reached.
- **Links** become `MeshFaceLink`s: `kind` = the NavMap link type, `cost` = its
  length, NaN (unspecified) for a negative length, which upstream means
  "compute it". A link to a NavCel out of range is dropped.
- **Dropped layers and links** are counted in the `/dropped_layers` and
  `/dropped_links` summary series and reported once per change through the
  host's parser diagnostics (`parser_navmap.dropped_layers`,
  `parser_navmap.dropped_links`).
- **`geometry_id`** becomes its decimal text; 0 or absent becomes "".

A NavMapLayer message becomes a `MeshMapUpdate` with one face patch for the
layer's channel, with the name, datatype and `ChannelInfo` a map gives the same
layer: dense (no indices) when it covers every NavCel, sparse with its indices
otherwise. An update for a name a map drops (empty or reserved) fails.

## Message layouts

The NavMap messages changed across releases (layers gained metadata, `int16`
data and sparse indices; the map gained `geometry_id` and links). The parser
compiles the `.msg` bundle each topic carries (`navmap_msg_decoder.hpp`) and
decodes by it, so every layout works and future fields are skipped by type. A
topic without a schema is decoded with the current layout.

Every array, fixed-size or not, is bounded by the remaining payload divided by
the fewest bytes one element takes (computed per type when the bundle is
compiled; a message type without fields is one `uint8` on the wire, as rosidl
writes it), so a hostile length or schema fails the message instead of
allocating far beyond the payload. A schema that nests messages more than 32
levels deep does not bind. Integer fields a hostile schema
declares as floating (`type`, the link NavCels, index arrays) are range-checked
before conversion; a value that does not fit drops its layer or link.

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
claims and the bind guard, the geometry and channels, every layer encoding (U8,
F32, F64, I16; unit, unknown value, sparse coverage, a type/array mismatch) in
maps and updates, the naming and datatype rules shared by the two, NavMapLayer
updates, `use_embedded_timestamp`, the scalar summaries and dropped counts, and
malformed input: every truncated prefix of a message (run it under
AddressSanitizer to catch an unchecked read), big-endian payloads, oversized
sequence lengths, arrays of field-less types and NaN in integer fields.

`tests/data/` holds messages serialized by rclpy from the real
`navmap_ros_interfaces` and recorded with rosbag2, for the current and the
first released layout: the schema text each MCAP channel carries (`*.msg`) and
the raw CDR payload of one NavMap and one NavMapLayer (`*.cdr`).

```bash
ctest --test-dir build -R navmap_parser
```
