# Changelog — parser_protobuf

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_protobuf/`.

## [1.2.0] - 2026-09-06

### Added
- Real `GridMap`/`Grid` message parsing.
### Fixed
- Wire-format bounds checks on malformed input.
- Import-root path mapping no longer maps the entire host filesystem as a proto import root.
