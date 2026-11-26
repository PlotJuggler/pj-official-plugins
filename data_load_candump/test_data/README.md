# Test fixtures

Small, hand-written, committed text files (no generator script, unlike `data_load_blf`'s binary `sample.blf`). Every
line shape is copied from the output of [linux-can/can-utils](https://github.com/linux-can/can-utils)
`candump.c`/`lib.c`; `candump_parser.hpp`'s header comment has the references.

- `log_format.log` — `candump -l` log format. Covers: a decodable standard frame (id `100`, `sample.dbc`'s `Speed`),
  a decodable extended frame (id `000004D2`, `ExtSig`), an RTR frame (`200#R`), an error frame (id bit `0x20000000`
  set), a CAN FD frame (`##`), a CAN XL frame (`00123#11:22:12345678#AABB`), the `-x` direction suffix (` R`), a
  `DROPCOUNT:` notification (no timestamp, no interface token), an interface with a dot and a dash (`can-eth0.1`) with
  a 9-digit (nanosecond, `-tN`) fraction, an empty-payload frame (id `500`), and a classic 8-byte frame with the
  optional raw len8_dlc suffix (`600#..._F`, `UnderscoreDlc`).
- `screen_format.txt` — interactive screen output; every line has the leading space real output has before the
  timestamp. Covers: a plain frame, the `-x` direction block (`  RX - -` / `  TX - -`; the flag-pair token has an
  embedded space, so it tokenizes as two symbols), a CAN FD frame with the 2-digit `[NN]` length marker, an error
  frame with the trailing `ERRORFRAME` word, a `-e` TAB-continuation detail line (`\tbus-off`), an RTR frame (`[N]`
  then the words `remote request`), a CAN XL frame (`123 [0004] (00|11:22:12345678) AA BB`), the LEN8_DLC brace
  marker (`{F}`, `UnderscoreDlc`), the 5-space SFF indent applied once an EFF/error frame has been seen, and a short
  (right-padded) interface name.
- `n2k.log` + `n2k.dbc` — J1939 / NMEA 2000. The DBC is the one in `common/can_dbc/tests/j1939_test.cpp` plus
  `LateStart`: ids carry the Vector flag and source address `0xFE`, as exporters write them. The capture covers one PGN
  from two sources with a priority change (`VesselHeading`, 0x10 and 0x04), two interleaved fast packets of one PGN
  from two sources, one 20 and one 10 bytes long (`PositionLike`, its `Tail` signal only in the long one), a fast
  packet missing frame 1, a PDU1 request to destination 0x23 (`ISORequest`), a PGN not in the DBC, a standard frame
  (`StdMsg`), and a 1-byte fast packet that ends before its message's only signal (`LateStart`).
- `relative_tz.log` / `relative_td.log` — the same frame repeated with monotonically increasing small timestamps
  (`-t z`, `%03llu` zero-padded seconds) and with a non-monotonic sequence (`-t d`, delta since the previous frame), so
  `candump_detail::detectTimeMode` picks `kRelativeMonotonic` / `kRelativeDelta`.
- `relative_td_mixed.log` / `relative_td_mixed_screen.txt` — the same `-t d` capture in log and screen shape: data
  frames (id `100`, Speed 100/200/1000/300) interleaved with an RTR, an error frame (in the screen file with its `-e`
  TAB detail line), a CAN FD frame, a CAN XL frame and (log file only) a `DROPCOUNT:` line. Every timestamped line
  carries a delta, so the data frames land at 0 / 0.3 / 0.6 / 0.95 s only if the running sum advances over the
  non-data lines too.
- `sample.dbc` — the DBC for the `log_format.log`/`screen_format.txt` decode tests: `EngineData` (id `0x100`, `Speed`
  at bytes 0-1, LE, x0.1), `ExtMsg` (id `0x4D2` extended, `ExtSig` at byte 0), and `UnderscoreDlc` (id `0x600`, `Under`
  at byte 0, DLC 8; matches both the `_F`-suffixed log frame and the `{F}`-braced screen frame). It also carries two
  `VAL_` tables used by `CandumpDecode.ValueTableLabelsRouteThroughSignalRowBuilder`: `Speed`'s key (`1000`) matches
  its decoded raw value (label found), `ExtSig`'s key (`5`) does not match its raw value (`1`), so its
  `<signal>_label` falls back to the number as text.
- `arus_subset.csv` — a subset of the real [ARUS FS dictionary](https://github.com/ARUSfs/log_plotter/blob/main/can_conversions.csv),
  covering the CSV -> DBC ID-splitting rule (`0x1a31`/`0x1a32` -> base `0x1a3`, two signals in one message) and a
  negative-scale row (`extensometer`, exercises the common/can_dbc regex fix).
- `arus_race.txt` — a `candump -l` capture using `arus_subset.csv`'s ids and byte layouts. The expected values
  (`IMU_ax -2.0`, `IMU_ay 2.0`, `fl_inv_speed 10.47197551196`, `fl_inv_torque 9.8`, `brake_hydr_front -40.9239940387`
  then `-100.5365126677`, `extensometer 0.445271946587`) were recomputed independently in Python against the real CSV.
