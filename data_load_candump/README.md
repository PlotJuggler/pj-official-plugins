# candump Data Loader

Imports [Linux SocketCAN `candump`](https://github.com/linux-can/can-utils) captures and decodes them to timeseries:
both the `candump -l` log format and interactive screen output redirected to a file. The text is parsed by hand (no
`<regex>`, no locale-dependent timestamp parsing). Signal decoding uses the shared `pj_can_dbc` library
([`dbc_parser_cpp`](https://github.com/LinuxDevon/dbc_parser_cpp), MIT), the same decoder as the MF4 and BLF loaders.

The line grammar follows the `linux-can/can-utils` sources (`candump.c`, `lib.c`); `candump_parser.hpp` has the exact
references.

## Features

- Reads both candump shapes, per line (a capture need not be homogeneous):
  - **Log** (`candump -l`): `(ts) iface ID#DATA`, with `##<flag>DATA` for CAN FD, `#R[dlc]` (optionally with a
    `_<hex>` raw-DLC suffix) for RTR, `%02X%03X#%02X:%02X:%08X#<data>` (vcid+prio#flags:sdt:af#data) for CAN XL, and
    `-x`'s trailing ` R`/` T`.
  - **Screen** (interactive output): a leading space before the timestamp (log format never has one), the interface,
    an optional `-x` direction block (`RX`/`TX` plus a flag-pair token pair, e.g. `- -`), the id, then a length
    marker: `[N]` (classic), `{H}` (raw-DLC view; the payload is however many data-byte tokens follow), `[NN]`
    (CAN FD) or `[NNNN]` (CAN XL, followed by a `(vcid|flags:sdt:af)` group instead of data). RTR shows `[N]` and
    the words `remote request`. The extra 5-space indent and padded interface names are plain whitespace.
- **Per-interface dictionary**: assign a `.dbc` or an ARUS-style CSV (see below, translated to DBC text in memory) to
  each interface. Decoded signals become one topic per `(interface, message)`: `CAN/{interface}/{message}`.
- A signal with a DBC `VAL_` value table also gets a **`<signal>_label`** text field (the label, or the raw value as
  text when unmatched); see `common/can_dbc`'s `signal_row.hpp`. It is `<signal>_label_2` (then `_3`, ...) when the
  message already has a signal named `<signal>_label`.
- **J1939 / NMEA 2000** (per interface, next to its dictionary): an extended frame finds its DBC message by **PGN**,
  so one DBC message decodes that parameter group from every source address and priority (a DBC stores one sample id
  per message).
  - Topics name the source, `CAN/{interface}/{message}/0x{SA}`, and for a destination-specific (PDU1) message the
    destination, `CAN/{interface}/{message}/0x{SA}/0x{DA}`: the same PGN from two GNSS receivers stays in two topics,
    while a priority change does not split one. Standard (11-bit) frames still match by id.
  - When a DBC defines one PGN several times (once per sender, each with its own layout), the message whose id names
    the frame's source address (and PDU1 destination) is used, else the first one.
  - **NMEA 2000 fast packets**: a DBC message longer than 8 bytes is a fast packet. Its frames (up to 32, at most
    223 bytes) are reassembled per CAN id, so per source, and the message is recorded at its first frame's time. A
    frame that does not continue its message in order (a lost or repeated frame, a new sequence counter), or a
    message the capture ends in the middle of, is dropped and counted in the summary. The payload length varies per
    message (repeated fields), so a signal past the end of a shorter payload is left out of that row instead of
    failing it. Fast-packet frames never show up as raw bytes.
- Interfaces without an assigned dictionary, and frames a dictionary can't match or decode, optionally fall back to raw
  bytes: `CAN/{interface}/0x{id}/byte0..N` (a dialog toggle, on by default).
- RTR, CAN FD, CAN XL and error frames (`CAN_ERR_FLAG` set on the id; `ERRORFRAME` in screen format), a `-e` error
  frame's TAB-continuation detail lines, and `DROPCOUNT:` notifications are recognized and counted, never treated as
  malformed. `DROPCOUNT:` lines carry neither a timestamp nor a leading interface, so the interface and drop count are
  parsed from the message text, and such lines never count toward "this file has no timestamps".
- Timestamp mode (absolute, relative-monotonic `-t z`, or relative-delta `-t d`) is auto-detected from a bounded
  prescan (first timestamp `>= 1e9` seconds: absolute; otherwise monotonic-vs-not decides `-t z` vs `-t d`), with a
  manual override in the dialog. Relative timestamps carry no wall-clock information: align the dataset against other
  sources in the Source Timeline manually.

## ARUS CSV Dictionary

Semantics follow [ARUSfs/log_plotter](https://github.com/ARUSfs/log_plotter)'s `read_can_txt_file` and its
[`can_conversions.csv`](https://github.com/ARUSfs/log_plotter/blob/main/can_conversions.csv):

- `bitIn`/`bitFin` are **inclusive byte indices** into the CAN payload (not bit indices), always little-endian.
- Physical value = `raw * Scale + Offset`. `Power` is ignored.
- `Signed` is a bool (`True`/`False`, `1`/`0`, `yes`/`no`).
- An `ID` of **exactly 4 hex digits** is a base id (its first 3 digits) plus a per-row signal index (the 4th digit,
  whose value does not matter). Any other digit count is a single-signal message with that id. Rows sharing a base id
  become one CAN message with one signal per row; its DLC is `max(bitFin) + 1`; a base id above `0x7FF` is extended.

Invalid rows (non-hex `ID`, `bitIn > bitFin`, `bitFin > 7`, an unparseable `Signed`/`Scale`/`Offset`) are skipped with
a warning; the rest of the dictionary loads.

## Timestamp Parsing

The leading `(seconds.fraction)` token is parsed by structure (integer seconds + a fraction scaled to nanoseconds by
digit count), so microsecond (6-digit, the default) and nanosecond (9-digit, `-tN`) fractions both work. `-t d` deltas
accumulate over every line that carries a timestamp (RTR, error, CAN FD/XL and unparsable lines included), as candump
prints them.

## Configuration

```json
{
  "filepath": "/path/to/capture.log",
  "iface_dicts": { "can0": ["powertrain.dbc"], "vcan1": ["dictionary.csv"] },
  "aux_base_dir": "/path/to",
  "iface_protocols": { "can1": "nmea2000" },
  "raw_unassigned": true,
  "time_mode": 0
}
```

`iface_protocols`: `"j1939"` or `"nmea2000"` per interface; an interface without an entry is raw CAN (matched by the
whole id).

`time_mode`: `0` auto-detect, `1` force absolute, `2` force relative monotonic (`-t z`), `3` force relative delta
(`-t d`). A forced mode is saved with `time_mode_file`, the name of the capture it was chosen for, and applies to
captures of that name only, whichever folder they lie in (so a layout reopens it from a moved folder or an upload); the
next file opened with the loader goes back to auto-detection.

Dictionary paths are portable: a dictionary inside the data file's folder is saved relative to it
(`"powertrain.dbc"`, `"dbc/powertrain.dbc"`), so a moved or copied layout folder decodes with the dictionary beside the
data; other paths are saved as given. `aux_base_dir` records the data file's folder at save time, so settings reused
for a different data file still find relative entries. An absolute path under a folder that has moved (for example
`dbc/` next to `data/`) is rebased onto the data file that is open.

## Testing

Tests are hermetic: the fixtures under `test_data/` are small hand-written text files (see `test_data/README.md`).

```bash
ctest --test-dir <build> -R 'candump|can_dbc'
```

## Known Limitations

- `-t a` (wall-clock, localized date/time) timestamps are recognized but not decoded: those lines are counted and
  reported, not imported (there is no reliable, tool-independent way to parse localized date/time text).
- CAN XL payloads are never decoded (frames are recognized and counted, like CAN FD).
- A DBC message with a signal wider than 64 bits (for example the NMEA 2000 text fields of `ProductInformation` or the
  AIS static-data names) is dropped whole at load, its numeric signals included.
- J1939 multi-frame transport (TP.CM / TP.DT / BAM) is not reassembled: a J1939 message longer than 8 bytes does not
  decode.
- An NMEA 2000 fast-packet PGN that the DBC describes with a message of 8 bytes or less is not reassembled: each of
  its frames decodes on its own, with wrong values.
- Multiplexed CAN signals are not decoded (a `dbc_parser_cpp` limitation, shared with the MF4/BLF loaders).
- `.log`/`.txt` are not exclusive to this loader: PlotJuggler picks the first plugin that claims an extension, with no
  content sniffing. Use the `.candump` extension to avoid ambiguity, or expect a "this does not look like a candump
  capture" message from the dialog if the wrong plugin claims the file.
