# Test fixtures

All files here are small, hand-written, and committed (no generator script is
needed — unlike `data_load_blf`'s binary `sample.blf`, candump output is
plain text). Every line shape is copied from the ACTUAL output format
produced by [linux-can/can-utils](https://github.com/linux-can/can-utils)
`candump.c`/`lib.c` (verified against those sources, not guessed) — see
`candump_parser.hpp`'s top-of-file comment for exact function/line
references.

- `log_format.log` — `candump -l` log format (`lib.c
  snprintf_canframe`). Covers: a decodable standard frame (id `100`, decodes
  via `sample.dbc`'s `Speed` signal), a decodable extended frame (id
  `000004D2`, `ExtSig`), an RTR frame (`200#R`), an error frame (id bit
  `0x20000000` set), a CAN FD frame (`##`), a CAN XL frame in its real shape
  (`00123#11:22:12345678#AABB` — `%02X%03X#%02X:%02X:%08X#<data>`, vcid+prio
  then flags:sdt:af then data), the `-x` direction suffix (` R`), a
  `DROPCOUNT:` notification in its real shape (no timestamp, no leading
  interface token — `DROPCOUNT: dropped 3 CAN frames on 'can0' socket (total
  drops 3)`), an interface with a dot and a dash (`can-eth0.1`) together with
  a 9-digit (nanosecond, `-tN`) timestamp fraction, an empty-payload frame
  (id `500`), and a classic 8-byte frame with the optional raw len8_dlc
  suffix (`600#..._F`, decodes via `UnderscoreDlc`).
- `screen_format.txt` — interactive candump screen output (`lib.c
  snprintf_long_canframe`). Every line has the leading space real candump
  output always has before the timestamp (log format never does). Covers: a
  plain frame, the `-x` direction block (`  RX - -` / `  TX - -` — note the
  flag-pair token itself has an embedded space, so it tokenizes as two
  symbols), a CAN FD frame with the real 2-digit `[NN]` length marker, an
  error frame with the trailing `ERRORFRAME` word, a `-e` TAB-continuation
  error-detail line (`\tbus-off`) on its own following line, an RTR frame
  (`[N]` length marker followed by the literal words `remote request`), a
  CAN XL frame in its real shape (`123 [0004] (00|11:22:12345678) AA BB`),
  the LEN8_DLC brace length marker (`{F}`, decodes via `UnderscoreDlc`), the
  5-space SFF-indent applied once an EFF/error frame has been seen in a
  session, and a short (right-padded) interface name.
- `relative_tz.log` / `relative_td.log` — same frame repeated with,
  respectively, monotonically increasing small timestamps (`-t z` shape,
  `%03llu` zero-padded seconds) and a non-monotonic sequence (`-t d`,
  delta-since-previous-frame shape) so `candump_detail::detectTimeMode`
  picks `kRelativeMonotonic` / `kRelativeDelta`.
- `sample.dbc` — the DBC used by `log_format.log`/`screen_format.txt`'s
  decode tests: `EngineData` (id `0x100`, `Speed` at bytes 0-1, LE, x0.1),
  `ExtMsg` (id `0x4D2` extended, `ExtSig` at byte 0), and `UnderscoreDlc`
  (id `0x600`, `Under` at byte 0, DLC 8 — matches both the log format's
  `_F`-suffixed frame and the screen format's `{F}`-braced frame). Also
  carries two `VAL_` value tables exercised by
  `CandumpDecode.ValueTableLabelsRouteThroughSignalRowBuilder`: `Speed`'s
  table key (`1000`) matches its actual decoded raw value exactly (label
  found), while `ExtSig`'s table key (`5`) does NOT match its actual raw
  value (`1`), so its `<signal>_label` field falls back to the number as
  text — one table entry each, deliberately picked to cover both outcomes.
- `arus_subset.csv` — a subset of the real ARUS FS dictionary
  (<https://github.com/ARUSfs/log_plotter/blob/main/can_conversions.csv>,
  fetched and verified 2026-09-21), covering the CSV -> DBC ID-splitting rule
  (`0x1a31`/`0x1a32` -> base `0x1a3`, two signals in one message) and a
  negative-scale row (`extensometer`, exercises the common/can_dbc regex
  fix).
- `arus_race.txt` — a `candump -l` capture using `arus_subset.csv`'s ids and
  byte layouts; the expected decoded values (`IMU_ax -2.0`, `IMU_ay 2.0`,
  `fl_inv_speed 10.47197551196`, `fl_inv_torque 9.8`,
  `brake_hydr_front -40.9239940387` then `-100.5365126677`,
  `extensometer 0.445271946587`) were independently recomputed in Python
  against the real CSV and match exactly.
