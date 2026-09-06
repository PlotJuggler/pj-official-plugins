# Changelog — data_load_mcap

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_mcap/`.

## [1.2.0] - 2026-09-06

### Added
- Extract a dataset-metadata document during import: file summary facts, every
  file-level MCAP Metadata record, and parsed views of PlotJuggler's
  `pj.capture` capture manifest (source descriptor recovered from its identity
  framing) and `pj.recording` session records. Displaying it in the dataset
  Info dialog requires a PlotJuggler build with the matching host support;
  older hosts load files exactly as before.

## [1.1.3] - 2026-08-09
