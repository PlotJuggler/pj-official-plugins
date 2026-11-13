# Changelog — parser_navmap

One entry per released version (newest first).

## [0.1.0] - 2026-11-13

### Added
- Show EasyNavigation NavMap maps (`navmap_ros_interfaces/msg/NavMap`) in the 3D
  scene, with every layer available for colouring there, in its own type and
  with its unit and "unknown" value.
- Apply the NavMapLayer updates published beside a map (e.g. on
  `navmap_updates`), so live maps change over time.
- Keep the links between NavCels (stairs, ramps, elevators, doors) for the 3D
  scene to draw.
- Plot a compact summary per message (header stamp and frame, vertex, NavCel,
  surface, layer and link counts, geometry id, and how many layers and links
  were dropped; for layers the name and value count) instead of the raw arrays.
  Dropped layers and links are also reported as parser warnings.
- Honour the data source's "use embedded timestamp" option: maps, updates and
  their summaries are placed at the header stamp when it is set.
- Name each layer's channel after the layer; a layer with an empty, repeated
  or reserved name is dropped, so maps and their updates always address the
  same channel with the same type. A sparse integer layer inside a map without
  an "unknown" value to mark the unlisted NavCels is dropped. NavCels on no
  surface are marked as no-data in the `surface` channel.
- Decode every released message layout, using the definition carried by each
  recording.
