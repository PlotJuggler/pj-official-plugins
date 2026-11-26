# Changelog — parser_protobuf

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_protobuf/`.

## Upcoming

### Fixed
- On Windows, a .proto file in a folder or with a name that has non-ASCII characters (Cyrillic, Chinese, accents) compiles instead of failing with "Could not open file".

## [1.3.1] - 2026-11-17

### Fixed
- A field the sender left out of a message is no longer plotted as 0 (or as its
  proto2 default). This covers proto2 fields, proto3 `optional` fields, the
  inactive members of a oneof and unset sub-messages: the series has no sample
  for that message. Plain proto3 fields cannot be told apart from 0 and are
  unchanged.
- With "use embedded timestamp" on, a message that leaves out an `optional`
  timestamp field keeps its log/receive time instead of landing at 1970.

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
