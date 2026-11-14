# Changelog — data_load_csv

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_csv/`.

## [1.0.8] - 2026-11-08

### Changed
- Write records straight from the parsed columns instead of first copying
  every cell into one array to sort it. Same records, in the same order; a
  260 MB CSV now peaks at 622 MiB instead of 3.7 GiB and imports about 3x
  faster.

## [1.0.7] - 2026-09-28

### Changed
- Append rows by field handle instead of by name. Every cell used to copy its
  column name into the row and have the host resolve it again. About 30%
  faster on a 500 k-row, 31-column file. Imported data is unchanged.

## [1.0.6] - 2026-08-06
