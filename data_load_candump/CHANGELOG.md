# Changelog — data_load_candump

One entry per released version (newest first).

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
