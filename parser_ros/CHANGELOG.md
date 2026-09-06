# Changelog — parser_ros

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_ros/`.

## [1.2.0] - 2026-09-06

### Added
- `grid_map_msgs/GridMap` and foxglove `Grid` message mapping.
### Fixed
- ROS 1 headered topics: emit `/header/stamp` once — the duplicate column silently dropped every headered ROS 1 topic.
- Assorted parser hardening (bounds checks on malformed input).
