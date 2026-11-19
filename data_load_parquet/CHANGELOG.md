# Changelog — data_load_parquet

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_parquet/`.

## [1.0.5] - 2026-11-19

### Fixed
- Opening another Parquet file no longer pre-selects the wrong time column.
  PlotJuggler pre-fills the dialog with the choices made for the previous file,
  and the time column was remembered by its position in that file's schema: on a
  file with other columns, another column became the time axis. It is now
  remembered by name, so a file that has the column gets it back wherever it
  sits, and a file without it gets its timestamp column detected, as on a first
  open. This works with every PlotJuggler 4 version.
- Files written with zstd or lz4 compression (for example polars' default)
  load; they failed with "failed to create batch reader".
  Load errors now carry Arrow's reason, and a read error partway through a file
  is reported instead of ending the import early.

## [1.0.4] - 2026-11-16

### Fixed
- A time column stored as a time of day, a duration or a date (Parquet `TIME`, `DURATION` and `DATE`, as MATLAB writes timetables) now sets the time axis. Before, every row of such a file landed at time zero and plotted as a single point.

## [1.0.3] - 2026-11-09

### Changed
- Updated the bundled Apache Arrow from 23.0.1 to 26.0.1, picking up upstream bug and security fixes.

### Fixed
- Timestamps with a named time zone load faster: each zone is looked up once per file, not once per value. Where no time zone database is available (the browser app), this also stops a failed lookup per value.

## [1.0.2] - 2026-08-04
