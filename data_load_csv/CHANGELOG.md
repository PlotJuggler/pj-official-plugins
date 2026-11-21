# Changelog — data_load_csv

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_csv/`.

## Upcoming

### Fixed
- The warning above the preview (duplicate column names, rows skipped) reads in the dark theme: it was magenta on a
  white box, and the table's top-left corner was a light square. The warning now takes the theme's warning colour
  (PlotJuggler releases after 4.2.0; bold default text on earlier ones) and the corner follows the theme.

## [1.0.11] - 2026-11-19

### Fixed
- Opening another CSV file no longer pre-selects the wrong time column.
  PlotJuggler pre-fills the dialog with the choices made for the previous file,
  and the time column (and the date and time columns of "Multiple Columns")
  were remembered by their position in that file's header: on a file with other
  columns, another column became the time axis, or none was selected. They are
  now remembered by name, so a file that has the column gets it back wherever it
  sits, and a file without it opens on "Number of Rows". This works with every
  PlotJuggler 4 version.
- A time column that repeats a value (common in logs with one-second
  resolution) loads; the whole load used to fail with "duplicate field id".
- A custom time format whose fraction follows a separator other than `:`
  (`dd/MM/yyyy hh-mm-ss.zzz`) reads the fraction instead of dropping it, and a
  fraction of one or two digits (`.z`, `.zz`) is read too: a format ending in
  `.z` used to skip every row.

## [1.0.10] - 2026-11-18

### Fixed
- A numeric column whose first value is `nan`, `inf`, `-inf` or `Infinity` is
  no longer imported as text, which dropped all of its numbers and its curves.
  The column stays numeric, with NaN and infinite samples where those cells are.
- A row whose time column holds `nan` or `inf` is skipped as an invalid timestamp
  instead of being recorded at a garbage time.

## [1.0.9] - 2026-11-16

### Fixed
- A load in which every row is skipped (for example a custom time format that
  matches no row) now fails with "No rows imported" and the first skip reason,
  instead of succeeding with no data. Reloading a file this way used to silently
  remove the dataset already loaded; it is now kept.
- The "Rows Skipped" prompt no longer says rows have an incorrect number of
  columns when they were skipped for an invalid timestamp.

## [1.0.8] - 2026-11-08

### Changed
- Write records straight from the parsed columns instead of first copying
  every cell into one array to sort it. Same records, in the same order; a
  260 MB CSV now peaks at 622 MiB instead of 3.7 GiB and imports about 3x
  faster.
- Emphasised text in the dialog uses DemiBold (600) instead of Bold, matching the app.

### Fixed
- On Windows, files whose path contains non-ASCII characters (such as Chinese) open again;
  the path was read in the system code page instead of as UTF-8.

## [1.0.7] - 2026-09-28

### Changed
- Append rows by field handle instead of by name. Every cell used to copy its
  column name into the row and have the host resolve it again. About 30%
  faster on a 500 k-row, 31-column file. Imported data is unchanged.

## [1.0.6] - 2026-08-06
