# MF4 / MDF Data Loader

Imports [ASAM MDF](https://www.asam.net/standards/detail/mdf/) measurement files
(`.mf4`, `.mdf`, `.dat`) — the standard automotive measurement/logging format.

Parsing is done with [`ihedvall/mdflib`](https://github.com/ihedvall/mdflib)
(MIT); optional CAN bus-signal decoding uses
[`dbc_parser_cpp`](https://github.com/LinuxDevon/dbc_parser_cpp) (MIT). Both are
vendored via CPM and statically linked, so the plugin `.so` has no extra runtime
dependencies.

## Features

- Reads MDF v3 and v4, including `##DZ` compressed data blocks and unfinalized
  (`UnFinMF`) files (e.g. from CANedge loggers)
- One topic per channel group; one field per channel
- Numeric channels are imported as engineering (CC-conversion-applied) values;
  text channels as strings
- **DBC-based CAN decoding**: bus-logging channel groups are decoded into named
  physical signals when one or more `.dbc` databases are supplied (via config;
  see `dbc_paths`). One topic per CAN message, namespaced by the frame's bus
  channel (`CAN/ch<N>/<message>`, or `CAN/<message>` when the file records no
  bus channel), one field per signal. Standard and extended (29-bit / J1939)
  frames are disambiguated. Frames that match a message id but cannot be decoded
  (truncated, or a CAN FD payload > 8 bytes) are counted and reported, not
  silently dropped.
- Preview dialog listing the file's channel groups (name, sample count, channel
  count, bus type)

## Timestamp Handling

Absolute nanoseconds = the file header start time (`GetStartTime()`) plus the
channel group's master (time) channel offset. CAN frame times come from the
frame's own relative timestamp added to the header start time.

## Testing

Unit tests are hermetic (they synthesize MF4 files in-memory with mdflib's
writer and use an inline DBC), so they need no external files. Use a filter that
covers both the MF4 reader tests and the shared CAN/DBC decoder tests:

```bash
ctest --test-dir <build> -R "mf4|can_dbc"
```

Two additional **integration tests** run against real files but are **skipped**
unless the environment variable `MF4_TEST_DATA_DIR` points at a directory
containing the two sample files below. The files are **not** committed (they are
third-party assets and larger than the repo's binary-fixture budget) — download
them from their upstream homes:

| File | ~Size | Download |
|------|-------|----------|
| `ASAP2_Demo_V171.mf4` (finalized measurement, named signals + conversions) | 1.2 MB | `https://raw.githubusercontent.com/danielhrisca/asammdf/master/test/asammdf/gui/resources/ASAP2_Demo_V171.mf4` |
| `canedge_00000001.MF4` (unfinalized CANedge raw-CAN log, J1939) | 3.0 MB | `https://raw.githubusercontent.com/CSS-Electronics/api-examples/master/examples/other/asammdf-basics/input/00000001.MF4` |

```bash
mkdir -p /tmp/mf4_samples && cd /tmp/mf4_samples
curl -sL -O https://raw.githubusercontent.com/danielhrisca/asammdf/master/test/asammdf/gui/resources/ASAP2_Demo_V171.mf4
curl -sL -o canedge_00000001.MF4 https://raw.githubusercontent.com/CSS-Electronics/api-examples/master/examples/other/asammdf-basics/input/00000001.MF4

MF4_TEST_DATA_DIR=/tmp/mf4_samples ctest --test-dir <build> -R mf4 --output-on-failure
```

## Known Limitations

- Numeric channels are read as engineering doubles (`kFloat64`); native integer
  column types are not preserved.
- Out of scope in v1: enum (value→text) channels, multidimensional channel
  arrays, multiplexed CAN signals, and correct epochs for *appended* files with
  multiple measurements at distinct start times.
- The dialog is a read-only preview. Interactive channel selection and a DBC
  file picker are follow-ups (they need dialog-engine input round-tripping); for
  now, DBC databases are supplied through the saved config (`dbc_paths`).
