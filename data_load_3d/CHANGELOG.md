# Changelog — data_load_3d

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_3d/`.

## Upcoming

### Fixed
- On Windows, a file in a folder or with a name that has non-ASCII characters (Cyrillic, Chinese, accents) opens instead of failing with "cannot open file".

## [1.2.0] - 2026-11-25

### Added
- LiDAR `.las` files (ASPRS LAS 1.0-1.4, point formats 0-10) load as point clouds, with intensity,
  returns, classification, scan angle, GPS time and colour per point. Georeferenced coordinates are
  re-centred and the offset is reported as `origin/x,y,z` in the summary.

## [1.1.0] - 2026-11-19

### Added
- Browser build: the loader is included in PlotJuggler's WebAssembly app.

### Changed
- The manifest description says that each file is a single snapshot, not a time series.

## [1.0.1] - 2026-08-04
