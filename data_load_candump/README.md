# candump Data Loader

Imports [Linux SocketCAN `candump`](https://github.com/linux-can/can-utils)
captures and decodes them to timeseries — both the `candump -l` log format
(`candump -l vcan0 > file.log`) and interactive screen output redirected to a
file. No vendored binary parser: the text is parsed by hand (no `<regex>`, no
locale-dependent number parsing of the timestamp). CAN signal decoding reuses
the shared `pj_can_dbc` library ([`dbc_parser_cpp`](https://github.com/LinuxDevon/dbc_parser_cpp),
MIT) — the same decoder as the MF4 and BLF loaders.

Every line shape below is verified directly against the
[linux-can/can-utils](https://github.com/linux-can/can-utils) sources that
produce it — `candump.c` (`main()`'s per-frame print loop) and `lib.c`
(`snprintf_canframe` for `-l`/log format, `snprintf_long_canframe` for the
interactive/screen format, `snprintf_can_error_frame` for `-e` error detail)
— not inferred or guessed; see `candump_parser.hpp`'s top-of-file comment for
exact line references into those files.

## Features

- Reads both candump shapes, per line (a capture is not assumed to be
  homogeneous):
  - **Log** (`snprintf_canframe`, always used for `-l` output): `(ts) iface
    ID#DATA`, with `##<flag>DATA` for CAN FD, `#R[dlc]` (optionally with a
    `_<hex>` raw-DLC suffix) for RTR, `%02X%03X#%02X:%02X:%08X#<data>`
    (vcid+prio#flags:sdt:af#data) for CAN XL, and `-x`'s trailing ` R`/` T`.
  - **Screen** (`snprintf_long_canframe`, interactive output): a LEADING
    SPACE before the timestamp (log format never has one), then `iface`,
    then an optional `-x` block ("  RX"/"  TX" plus a flag-pair token pair,
    e.g. "- -"), then `ID`, then a length marker — `[N]` (1 decimal digit,
    classic 0-8 bytes), `{H}` (1 hex digit, the LEN8_DLC raw-DLC view —
    payload length there is however many data-byte tokens actually follow,
    not the digit itself), `[NN]` (2 digits, CAN FD), or `[NNNN]` (4 digits,
    CAN XL, followed by a `(vcid|flags:sdt:af)` group instead of plain
    data) — and RTR shows the classic `[N]` marker followed by the two
    words `remote request` instead of bytes. An EFF/error id may be
    preceded by a further 5-space SFF indent once an EFF frame has been
    seen in the same capture; interface names are right-padded to the
    widest name in the session. Both are pure whitespace and need no
    special handling.
- **Per-interface dictionary**: assign a `.dbc` file or an ARUS-style CSV
  (`ID,bitIn,bitFin,Signed,Power,Scale,Offset,Name`, translated to DBC text
  in memory — see below) to each interface. Decoded signals become one topic
  per `(interface, message)` — `CAN/{interface}/{message}`.
- A signal decoded from a DBC `VAL_` value table also gets a
  **`<signal>_label`** text field (decoded label, or the raw value as text
  when unmatched) — see `common/can_dbc`'s `signal_row.hpp` for the full
  convention; collides with a DBC signal already named `<x>_label`.
- Interfaces without an assigned dictionary, and frames a dictionary can't
  match or decode, optionally fall back to raw bytes:
  `CAN/{interface}/0x{id}/byte0..N` (toggle in the dialog, on by default).
- RTR frames, CAN FD frames, CAN XL frames, error frames (`CAN_ERR_FLAG` set
  on the id, shown with a trailing `ERRORFRAME` word in screen format), a
  `-e` error frame's further TAB-continuation detail lines, and
  `DROPCOUNT:` drop notifications are all recognized and counted, never
  treated as malformed. `DROPCOUNT:` lines (`DROPCOUNT: dropped N CAN
  frame(s) on '<iface>' socket (total drops M)`) carry neither a timestamp
  nor a leading interface token in either candump output mode — the
  interface and drop count are parsed out of the message text instead, and
  such lines never count toward "this file has no timestamps".
- Timestamp mode — absolute, relative-monotonic (`-t z`), or relative-delta
  (`-t d`, the running sum of per-frame deltas) — is auto-detected from a
  bounded prescan of the file (first timestamp `>= 1e9` seconds -> absolute;
  otherwise monotonic-vs-not decides `-t z` vs `-t d`), with a manual
  override in the dialog. Relative timestamps do not carry wall-clock
  information — align the dataset against other sources in the Source
  Timeline manually.
- `-t a` wall-clock timestamps are recognized but **unsupported** in this
  version: such lines are counted with a warning, not decoded (there is no
  reliable, tool-independent way to parse arbitrary localized date/time text
  by structure alone — see Known Limitations).

## ARUS CSV Dictionary

Semantics verified against [ARUSfs/log_plotter](https://github.com/ARUSfs/log_plotter)'s
`read_can_txt_file` and its real
[`can_conversions.csv`](https://github.com/ARUSfs/log_plotter/blob/main/can_conversions.csv):

- `bitIn`/`bitFin` are **inclusive byte indices** into the CAN payload (not
  bit indices), always little-endian.
- Physical value = `raw * Scale + Offset`. `Power` is present in the CSV but
  unused.
- `Signed` is a bool (`True`/`False`, `1`/`0`, `yes`/`no`).
- An `ID` written with **exactly 4 hex digits** is ID-base (its first 3 hex
  digits) plus a per-row signal index (the 4th digit — its value doesn't
  matter, it only exists so each row's `ID` column is distinct for a
  multi-signal message). Any other digit count is used as-is: a
  single-signal message with that id. Rows sharing the same base id become
  one CAN message with one signal per row; the message's DLC is
  `max(bitFin) + 1`; a base id above `0x7FF` is emitted as extended.

Invalid rows (non-hex `ID`, `bitIn > bitFin`, `bitFin > 7`, an unparseable
`Signed`/`Scale`/`Offset`) are skipped with a warning, not fatal to the rest
of the dictionary.

## Timestamp Parsing

The leading `"(seconds.fraction)"` token is parsed by structure (integer
seconds + a fractional part scaled to nanoseconds by digit count), not by
locale-dependent floating-point parsing — this also means both microsecond
(6-digit, candump's default) and nanosecond (9-digit, `-tN`) fractions are
handled without special-casing.

## Configuration

```json
{
  "filepath": "/path/to/capture.log",
  "iface_dicts": { "can0": ["powertrain.dbc"], "vcan1": ["dictionary.csv"] },
  "raw_unassigned": true,
  "time_mode": 0
}
```

`time_mode`: `0` auto-detect, `1` force absolute, `2` force relative
monotonic (`-t z`), `3` force relative delta (`-t d`).

## Testing

Tests are hermetic — all fixtures under `test_data/` are small, hand-written
plain-text files (see `test_data/README.md`), no external downloads needed.

```bash
ctest --test-dir <build> -R 'candump|can_dbc'
```

## Known Limitations

- `-t a` (wall-clock, localized date/time) timestamps are recognized but not
  decoded in this version — those lines are counted and reported, not
  imported.
- CAN XL payloads are never decoded (frames are recognized and counted, like
  CAN FD) — out of scope for this version, same as CAN FD.
- Multiplexed CAN signals are not decoded (a `dbc_parser_cpp` limitation,
  shared with the MF4/BLF loaders).
- `.log`/`.txt` are not exclusive to this loader: PlotJuggler picks the
  first plugin that claims a given extension, with no content sniffing at
  the file-picker level. Use the dedicated `.candump` extension to avoid any
  ambiguity, or expect a clear "this does not look like a candump capture"
  message from the dialog if the wrong plugin claims the file.
