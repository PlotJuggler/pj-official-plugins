# Changelog — parser_json

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_json/`.

## [1.1.1] - 2026-09-22

### Fixed
- Non-negative JSON integers are now emitted as int64 (like negative ones), so a field whose sign changes across messages keeps a single column type; before, the datastore rejected every message whose sign differed from the first one it saw for that field. Integers above `INT64_MAX` are emitted as double instead of wrapping negative.

## [1.1.0] - 2026-08-04
