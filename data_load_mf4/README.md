# MF4 / MDF Data Loader

Imports [ASAM MDF](https://www.asam.net/standards/detail/mdf/) measurement files (`.mf4`, `.mdf`, `.dat`).

Parsing uses [`ihedvall/mdflib`](https://github.com/ihedvall/mdflib) (MIT); optional CAN bus-signal decoding uses
[`dbc_parser_cpp`](https://github.com/LinuxDevon/dbc_parser_cpp) (MIT). Both are vendored via CPM and statically
linked, so the plugin `.so` has no extra runtime dependencies.

## Features

- Reads MDF v3 and v4, including `##DZ` compressed data blocks and unfinalized (`UnFinMF`) files (e.g. from CANedge
  loggers).
- One topic per channel group, one field per channel. Numeric channels are imported as engineering
  (conversion-applied) values, text channels as strings.
- **DBC-based CAN decoding**: bus-logging channel groups are decoded into named physical signals when one or more
  `.dbc` databases are supplied via the config (`dbc_paths`). One topic per CAN message, namespaced by the frame's bus
  channel (`CAN/ch<N>/<message>`, or `CAN/<message>` when the file records no bus channel), one field per signal.
  - Standard and extended (29-bit / J1939) frames are disambiguated. CAN FD frames decode like classic ones,
    including signals past byte 7.
  - Frames that match a message id but cannot be decoded (shorter than the message, or a signal reaching past the
    payload) are counted and reported.
  - A signal with a DBC `VAL_` value table also gets a **`<signal>_label`** text field (the label, or the raw value as
    text when unmatched); see `common/can_dbc`'s `signal_row.hpp`. It is `<signal>_label_2` (then `_3`, ...) when the
    message already has a signal named `<signal>_label`. MDF's own value-to-text channel conversion is not applied.
- A preview dialog lists the file's channel groups (name, sample count, channel count, bus type).

## Timestamps

Absolute nanoseconds: the file header start time (`GetStartTime()`) plus the channel group's master (time) channel
offset. CAN frame times are the frame's own relative timestamp plus the header start time.

## Configuration

```json
{
  "filepath": "/path/to/log.mf4",
  "dbc_paths": ["powertrain.dbc", "chassis.dbc"],
  "aux_base_dir": "/path/to"
}
```

Dictionary paths are portable: a dictionary inside the data file's folder is saved relative to it
(`"powertrain.dbc"`, `"dbc/powertrain.dbc"`), so a moved or copied layout folder decodes with the dictionary beside the
data; other paths are saved as given. `aux_base_dir` records the data file's folder at save time, so settings reused
for a different data file still find relative entries. An absolute path under a folder that has moved (for example
`dbc/` next to `data/`) is rebased onto the data file that is open.

## Testing

Unit tests are hermetic (they synthesize MF4 files with mdflib's writer and use an inline DBC). The filter covers
both the MF4 reader tests and the shared CAN/DBC decoder tests:

```bash
ctest --test-dir <build> -R "mf4|can_dbc"
```

Two **integration tests** run against real files and are skipped unless `MF4_TEST_DATA_DIR` points at a directory
containing the two sample files below. The files are not committed (third-party assets, larger than the repo's
binary-fixture budget); download them from upstream:

| File | ~Size | Download |
|------|-------|----------|
| `ASAP2_Demo_V171.mf4` (finalized measurement, named signals + conversions) | 1.2 MB | `https://raw.githubusercontent.com/danielhrisca/asammdf/master/test/asammdf/gui/resources/ASAP2_Demo_V171.mf4` |
| `canedge_00000001.MF4` (unfinalized CANedge raw-CAN log, J1939) | 3.0 MB | `https://raw.githubusercontent.com/CSS-Electronics/api-examples/master/examples/other/asammdf-basics/input/00000001.MF4` |

```bash
MF4_SAMPLES="$(mktemp -d)" && cd "$MF4_SAMPLES"
curl -sL -O https://raw.githubusercontent.com/danielhrisca/asammdf/master/test/asammdf/gui/resources/ASAP2_Demo_V171.mf4
curl -sL -o canedge_00000001.MF4 https://raw.githubusercontent.com/CSS-Electronics/api-examples/master/examples/other/asammdf-basics/input/00000001.MF4

MF4_TEST_DATA_DIR="$MF4_SAMPLES" ctest --test-dir <build> -R mf4 --output-on-failure
```

## Known Limitations

- Numeric channels are read as engineering doubles (`kFloat64`); native integer column types are not preserved.
- Not supported: enum (value-to-text) channels, multidimensional channel arrays, multiplexed CAN signals, and correct
  epochs for *appended* files with multiple measurements at distinct start times.
- The dialog is a read-only preview: there is no channel selection or DBC file picker; supply DBC databases through
  the saved config (`dbc_paths`).
