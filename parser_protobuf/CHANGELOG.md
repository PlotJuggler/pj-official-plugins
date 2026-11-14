# Changelog — parser_protobuf

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_protobuf/`.

## [1.3.0] - 2026-11-14

### Added
- `foxglove.Log` and `PJ.Log` messages are published as log objects (level,
  message and the `name` field as source) for the log viewer. Messages of 100
  characters or more, which the plottable columns leave out, are kept whole.
  The plottable columns are unchanged.

## [1.2.1] - 2026-11-03

### Fixed
- MCAP files whose protobuf schema lists a file before the files it imports, as the
  MCAP C++ example writes them, open again. Every channel of such a file was rejected
  with "message type not found".

## [1.2.0] - 2026-09-06

### Added
- Real `GridMap`/`Grid` message parsing.
### Fixed
- Wire-format bounds checks on malformed input.
- Import-root path mapping no longer maps the entire host filesystem as a proto import root.
