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
  surface, layer and link counts, geometry id; for layers the name and value
  count) instead of the raw arrays.
- Decode every released message layout, using the definition carried by each
  recording.
