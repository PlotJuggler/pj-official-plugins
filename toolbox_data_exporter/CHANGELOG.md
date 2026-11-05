# Changelog — toolbox_data_exporter

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_data_exporter/`.

## [0.114.3] - 2026-11-05

### Fixed
- Fix Windows ARM64 release builds by using only the Boost headers required by CSV/Parquet export dependencies.

## [0.114.2] - 2026-11-01

### Fixed
- With two recordings carrying the same series name, a drop exports the one the user dragged; it could export the
  other, with no warning. Its row and column name the recording, and each recording's topic goes to its own file. On a
  PlotJuggler that does not say which one was dragged, the drop is refused with that reason.
- Export stops and names any table row no loaded data answers, instead of writing a file without that column.

## [0.114.1] - 2026-08-04
