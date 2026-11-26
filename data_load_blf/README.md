# BLF Data Loader

Imports [Vector BLF](https://www.vector.com/) CAN log files (`.blf`) and decodes them to timeseries using DBC
databases.

BLF reading uses [`PetStr/lblf`](https://github.com/PetStr/lblf) (MIT); signal decoding uses the shared `pj_can_dbc`
library ([`dbc_parser_cpp`](https://github.com/LinuxDevon/dbc_parser_cpp), MIT), the same decoder as the MF4 loader.
Both are vendored via CPM and statically linked, so the plugin `.so` has no extra runtime dependencies.

## Features

- Reads classic CAN frames (`CAN_MESSAGE` / `CAN_MESSAGE2`), including zlib-compressed `LogContainer`s.
- **Per-channel DBC mapping**: assign a different `.dbc` to each CAN channel (BLF logs are multi-bus). Decoded signals
  become one topic per `(channel, message)`: `CAN/ch{N}/{message}`.
- The dialog has a DBC picker per channel (1-4) and a preview of the channels the file contains; more channels are set
  via the saved config.
- Standard and extended (29-bit / J1939) frames are disambiguated. Unmatched frames, matched frames shorter than their
  message, and channels without a DBC are counted and reported.
- A signal with a DBC `VAL_` value table also gets a **`<signal>_label`** text field (the label, or the raw value as
  text when unmatched); see `common/can_dbc`'s `signal_row.hpp`. It is `<signal>_label_2` (then `_3`, ...) when the
  message already has a signal named `<signal>_label`.

## Timestamps

Absolute nanoseconds: the file's measurement-start time (a Windows `SYSTEMTIME`, treated as UTC) plus each object's
relative timestamp (10 µs or 1 ns ticks, per the object-flags unit bit).

## Configuration

```json
{
  "filepath": "/path/to/log.blf",
  "channel_dbcs": { "1": ["powertrain.dbc"], "2": ["chassis.dbc"] },
  "aux_base_dir": "/path/to"
}
```

Dictionary paths are portable: a dictionary inside the data file's folder is saved relative to it
(`"powertrain.dbc"`, `"dbc/powertrain.dbc"`), so a moved or copied layout folder decodes with the dictionary beside the
data; other paths are saved as given. `aux_base_dir` records the data file's folder at save time, so settings reused
for a different data file still find relative entries. An absolute path under a folder that has moved (for example
`dbc/` next to `data/`) is rebased onto the data file that is open.

## Testing

Tests are hermetic: a 250-byte `sample.blf` generated with python-can is committed under `test_data/` (regenerate with
`test_data/gen_blf.py`).

```bash
ctest --test-dir <build> -R 'blf|can_dbc'
```

## Known Limitations

- CAN FD, LIN and other non-classic objects are skipped and counted (lblf has no CAN-FD parse struct).
- Multiplexed CAN signals are not decoded (a `dbc_parser_cpp` limitation).
- The dialog exposes channels 1-4; others are configured via the saved `channel_dbcs` map.
