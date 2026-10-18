# Changelog — parser_data_tamer

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_data_tamer/`.

## [0.10.0] - 2026-10-18

### Added
- Read data_tamer 2.x recordings with line schemas (version 5) and YAML schemas
  (version 6), including nested fields. Data_tamer 1.x recordings (version 4)
  remain supported.

### Changed
- Schema texts from releases before data_tamer 0.7.0, without a `### version:`
  line, are now rejected with a clear error.
- Preserve boolean values as booleans and character wire bytes as unsigned 8-bit
  integers, alongside the existing native integer and floating-point types.

### Fixed
- Reject malformed snapshot framing, truncated fields and payloads that do not
  match the schema, without appending partial records.

## [0.9.2] - 2026-09-28

### Changed
- Direct ingest appends snapshots by field handle, cached by schema field
  name. It no longer builds a `"/" + name` string and a second copy of it per
  field per message, nor makes the host resolve every name again. About 70%
  faster on 100 k snapshots of 60 fields. Imported data is unchanged.

## [0.9.1] - 2026-08-04
