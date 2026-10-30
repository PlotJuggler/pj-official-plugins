# Changelog — parser_data_tamer

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `parser_data_tamer/`.

## [0.9.3] - 2026-10-01

### Fixed
- Accept the full 64-bit range of DataTamer schema hashes on Windows. The
  dependency uses `stoull` instead of the platform-dependent-width `stoul`.
- Temporarily pin the reviewed dependency commit from data_tamer PR #92 until
  its release is published.

## [0.9.2] - 2026-09-28

### Changed
- Direct ingest appends snapshots by field handle, cached by schema field
  name. It no longer builds a `"/" + name` string and a second copy of it per
  field per message, nor makes the host resolve every name again. About 70%
  faster on 100 k snapshots of 60 fields. Imported data is unchanged.

## [0.9.1] - 2026-08-04
