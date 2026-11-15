# Changelog — data_load_parquet

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_parquet/`.

## [1.0.3] - 2026-11-09

### Changed
- Updated the bundled Apache Arrow from 23.0.1 to 26.0.1, picking up upstream bug and security fixes.

### Fixed
- Timestamps with a named time zone load faster: the zone is looked up once per column, not once per value. Where no time zone database is available (the browser app), this also stops a failed lookup per value.

## [1.0.2] - 2026-08-04
