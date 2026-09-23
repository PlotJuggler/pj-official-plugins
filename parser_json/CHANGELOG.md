# Changelog — parser_json

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_json/`.

## [1.2.1] - 2026-09-23

First release of the changes below; 1.2.0 was never published (the number collides with a stale GitHub release).

### Added
- The options dialog now lets you set the name of the embedded timestamp field (it was fixed to `timestamp`).

### Fixed
- Non-negative JSON integers are now emitted as int64 (like negative ones), so a field whose sign changes across messages keeps a single column type; before, the datastore rejected every message whose sign differed from the first one it saw for that field. Integers above `INT64_MAX` are emitted as double instead of wrapping negative.
- An empty or blank `timestamp_field_name` in a saved config now means the default `timestamp` instead of silently disabling the embedded timestamp.

## [1.1.0] - 2026-08-04
