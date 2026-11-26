# Changelog — data_load_mcap

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_load_mcap/`.

## Upcoming

### Fixed
- Opening an MCAP whose index has many chunks (one per message, hundreds of thousands in total) no longer freezes the window while the dialog reads it: the dialog takes its channels, schemas and message counts from the summary's offset records instead of parsing every chunk index.

## [1.3.4] - 2026-11-24

### Changed
- The tooltip of an always-loaded transform channel reads "Always loaded: required for 3D transform rendering.", the same as in the streamers' dialogs.

## [1.3.3] - 2026-11-23

### Changed
- Messages read back from the file after the import (images, point clouds) load about a fifth faster: their chunk is decompressed once and the message is read at its recorded offset, without copying and indexing the rest of the chunk.

## [1.3.2] - 2026-11-19

### Fixed
- A topic whose messages fail to decode no longer aborts the whole import. Failures are counted per topic; a topic with 100 failures in a row is skipped while the others keep loading. Each topic with failures is reported once, with its count and first error.
- A file whose every message fails to decode reports "MCAP import loaded no messages" and no longer leaves an empty topic behind.

## [1.3.1] - 2026-11-08

### Fixed
- Reject a chunk that declares an uncompressed size above 2 GiB. Readers
  allocated that size before decompressing, so a crafted lz4 or zstd chunk
  made the import or the serial fallback scan attempt an arbitrary
  allocation and fail with an out-of-memory error.
- Open MCAP files larger than 2 GiB on 32-bit builds (WebAssembly): the file
  reader measured and sought the file with a 32-bit `long`, so opening one
  failed before the loader started.
- Reject an uncompressed chunk whose declared uncompressed size differs from
  the bytes it carries. Readers copied the declared size, so a crafted file
  read past the record into other memory and showed it as message data.

## [1.3.0] - 2026-09-22

### Added
- Open a split ROS 2 bag through its `metadata.yaml`: every `.mcap` split it
  lists loads as one dataset, with topics merged across files instead of one
  prefixed dataset per file. Bags stored as sqlite3 (`.db3`) or with per-file
  compression are rejected with a clear message.

## [1.2.0] - 2026-09-06

### Added
- Extract a dataset-metadata document during import: file summary facts, every
  file-level MCAP Metadata record, and parsed views of PlotJuggler's
  `pj.capture` capture manifest (source descriptor recovered from its identity
  framing) and `pj.recording` session records. Displaying it in the dataset
  Info dialog requires a PlotJuggler build with the matching host support;
  older hosts load files exactly as before.

## [1.1.3] - 2026-08-09
