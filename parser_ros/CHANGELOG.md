# Changelog — parser_ros

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_ros/`.

## [1.2.3] - 2026-09-28

### Fixed
- `sensor_msgs/CompressedImage`: accept `compressed_image_transport` PNG formats (`"<encoding>; png compressed ..."`) and formats that name only the raw encoding (e.g. `"16UC1"`), identifying the codec from the payload's magic bytes. A 16UC1 PNG is routed to the depth path. These topics previously failed with "unsupported CompressedImage format".

## [1.2.2] - 2026-09-12

### Changed
- Bumped `rosx_introspection` to 3.1.2: picks up the unsigned CDR sequence-length fix (rosx #47) and 3.1.1's in-position DDS `@key` path rendering.

## [1.2.1] - 2026-09-06

### Fixed
- `diagnostic_msgs/DiagnosticArray`: read `level` before `name` (the actual wire order in ROS 1 and ROS 2). Every DiagnosticArray message desynced and failed with "Buffer overrun in deserializeString".
- `diagnostic_msgs/DiagnosticArray`: repeated keys within one message are suffixed `key[1]`, `key[2]`, ... and empty keys are skipped, instead of the whole message failing with "duplicate field name".

## [1.2.0] - 2026-09-06

### Added
- `grid_map_msgs/GridMap` and foxglove `Grid` message mapping.
### Fixed
- ROS 1 headered topics: emit `/header/stamp` once — the duplicate column silently dropped every headered ROS 1 topic.
- Assorted parser hardening (bounds checks on malformed input).
