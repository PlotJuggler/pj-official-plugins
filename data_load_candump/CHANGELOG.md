# Changelog — data_load_candump

One entry per released version (newest first).

## Upcoming

### Fixed
- A signed 64-bit DBC signal is read as two's complement. A negative value used to show as a huge positive one.
- On Windows, a file in a folder or with a name that has non-ASCII characters (Cyrillic, Chinese, accents) opens instead of failing with "cannot open file".

## [0.1.2] - 2026-11-19

### Fixed

- A timestamp mode forced for one capture no longer applies to the next capture opened with the loader, where it overrode auto-detection and could collapse or reorder the samples. The mode is saved with the file name of the capture it was chosen for: the next file goes back to auto-detection, while reopening a capture of that name, or replaying a layout of it from another folder or an upload, keeps the mode. This works with every PlotJuggler 4 version.

## [0.1.1] - 2026-10-19

### Fixed

- Dictionaries beside the data are saved relative to it, so a moved or copied layout folder keeps decoding with the dictionary (.dbc or .csv) next to its data (absolute paths anywhere under the folder that moved, such as a `dbc/` folder next to `data/`, are rebased on replay).
- A missing or unreadable dictionary is reported as "DBC file not found" / "cannot read DBC file" (`.dbc`) or "dictionary file not found" / "cannot read dictionary file" (`.csv`) instead of a parse error.

## [0.1.0] - 2026-09-21

### Added

- Initial release: imports Linux SocketCAN `candump` captures, both the
  `candump -l` log format and interactive screen-output captures.
- Per-interface dictionary: a `.dbc` file or an ARUS-style CSV
  (`ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name`, translated to DBC text
  in memory) decodes frames into named signals; interfaces without a
  dictionary (or frames a dictionary doesn't match/can't decode) optionally
  fall back to raw `byte0..N` fields.
- Recognizes (and counts, without treating as malformed) RTR frames, CAN FD
  frames, CAN XL frames, error frames (including `-e` TAB-continuation error
  detail lines), `DROPCOUNT:` drop notifications, and `-t a` wall-clock
  timestamps (the last is unsupported in this version: counted with a
  warning, not decoded). Line shapes are verified directly against the
  linux-can/can-utils sources that produce them.
- Timestamp mode (absolute / relative-monotonic `-t z` / relative-delta
  `-t d`) is auto-detected from a bounded prescan of the file, with a manual
  override in the dialog.
- Dialog: per-interface frame/id counts and current dictionary from a capped
  prescan, a dictionary picker per interface, a raw-fallback toggle, and the
  time-mode override.
- A signal decoded from a DBC `VAL_` value table now also gets a
  `<signal>_label` text field (decoded label, or the raw value as text when
  unmatched); negative and all-unsigned `VAL_` key conventions are both
  accepted. See the README for the `_label` naming convention.
