# Changelog — parser_ros

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_ros/`.

## [1.3.1] - 2026-11-03

### Fixed
- MCAP messages with `ros2idl` schemas are recognized and decoded. The parser
  resolves bundled IDL files and includes, preserves include ordering, and
  normalizes ROS2 resource names for decoding and builtin-object classification.
- IDL annotations whose arguments hold quoted text with commas, parentheses or
  escaped quotes (rosidl `@verbatim` comments) parse, with rosx_introspection 3.1.3.
- Empty or truncated ROS2 CDR headers no longer crash the parser (null-buffer reads
  in rosx_introspection, fixed in 3.1.3). Marker objects and specialized handlers
  report a parser error; a generic message yields no fields.
- Specialized scalar handlers return decoder-initialization failures as parser
  errors instead of letting exceptions escape.

## [1.3.0] - 2026-11-03

### Added
- DataTamer text values (`std::string`, recorded by data_tamer 2.0.0 as the schema type
  `string`) from `data_tamer_msgs/Snapshot` are imported as text series.

### Changed
- DataTamer schemas and snapshots are decoded with data_tamer 2.0.0, the same parser as
  the DataTamer Parser plugin. Schemas of data_tamer 1.x (version 4) still decode.
- A snapshot whose payload does not match its schema is reported as an error instead of
  being dropped silently.

### Fixed
- Accept the full 64-bit range of DataTamer schema hashes on Windows. The schema text's
  hash was converted with the 32-bit `stoul` there.

## [1.2.5] - 2026-09-29

### Fixed
- Messages that the datastore rejects no longer register all of their fields.
  An example is a `tf2_msgs/TFMessage` that lists the same parent/child pair
  twice. In 1.2.4 such a message could add empty series, and it could set the
  type of a field whose value type varies between messages. It now affects
  columns exactly as it did before 1.2.4.
- The error for such a message names the repeated field again
  (`duplicate field name '…'`), as before 1.2.4, instead of a numeric field
  id.

## [1.2.4] - 2026-09-28

### Changed
- Direct ingest appends records by field handle. Handles are cached by the
  field's position in the flattened message (a repeat costs one string
  compare), so the host no longer normalizes, hashes and looks up every
  field name of every message. About 33% faster on 368 k real
  `sensor_msgs/Imu` messages. Records whose shape or types change still go
  through the by-name path, so accepted data and error messages are
  unchanged.

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
