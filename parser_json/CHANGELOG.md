# Changelog — parser_json

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_json/`.

## [1.2.0] - 2026-09-22

### Added
- The options dialog now lets you set the name of the embedded timestamp field (it was fixed to `timestamp`).

### Fixed
- An empty or blank `timestamp_field_name` in a saved config now means the default `timestamp` instead of silently disabling the embedded timestamp.

## [1.1.0] - 2026-08-04
