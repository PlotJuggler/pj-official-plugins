# Changelog — data_load_lerobot

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_lerobot/`.

## [1.0.5] - 2026-11-26

### Fixed
- On Windows, a dataset in a folder that has non-ASCII characters (Cyrillic, Chinese, accents) in its name opens, and its episodes and videos load, instead of failing with "cannot open".

## [1.0.4] - 2026-11-19

### Fixed
- Opening another dataset no longer pre-selects the episodes picked for the
  previous one. PlotJuggler pre-fills the dialog with the choices made for the
  last dataset, and the episode indices were kept wherever the new dataset had
  them. The selection is now saved with the dataset's folder name and applies to
  that dataset only: another dataset opens with every episode selected, while
  reopening the dataset, or replaying a layout of it, keeps the selection. This
  works with every PlotJuggler 4 version.
- Datasets whose Parquet files are written with zstd or lz4 compression (for
  example polars' default) load; they failed with "failed to create batch
  reader". Errors reading a Parquet file now carry Arrow's reason.

## [1.0.3] - 2026-11-09

### Changed
- Updated the bundled Apache Arrow from 23.0.1 to 26.0.1, picking up upstream bug and security fixes.
- Updated the bundled FFmpeg from 8.1 to 8.1.2, picking up upstream bug and security fixes.

## [1.0.2] - 2026-08-04
