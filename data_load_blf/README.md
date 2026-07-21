# BLF Data Loader

Imports [Vector BLF](https://www.vector.com/) (Binary Logging Format) CAN log
files (`.blf`) and decodes them to timeseries using DBC databases.

BLF reading uses [`PetStr/lblf`](https://github.com/PetStr/lblf) (MIT); CAN
signal decoding reuses the shared `pj_can_dbc` library
([`dbc_parser_cpp`](https://github.com/LinuxDevon/dbc_parser_cpp), MIT) — the
same decoder as the MF4 loader. Both are vendored via CPM and statically linked,
so the plugin `.so` has no extra runtime dependencies.

## Features

- Reads classic CAN frames (`CAN_MESSAGE` / `CAN_MESSAGE2`), including
  zlib-compressed BLF `LogContainer`s
- **Per-channel DBC mapping**: assign a different `.dbc` database to each CAN
  channel (BLF logs are multi-bus). Decoded signals become one topic per
  `(channel, message)` — `CAN/ch{N}/{message}`.
- A dialog with a DBC file picker per channel (1-4) and a preview of which
  channels the file contains; more channels can be set via the saved config.
- Standard and extended (29-bit / J1939) frames are disambiguated; unmatched
  frames and channels without a DBC are counted and reported.

## Timestamp Handling

Absolute nanoseconds = the file's measurement-start time (a Windows `SYSTEMTIME`,
treated as UTC) plus each object's relative timestamp (10 µs or 1 ns ticks per
the object-flags unit bit).

## Configuration

```json
{
  "filepath": "/path/to/log.blf",
  "channel_dbcs": { "1": ["powertrain.dbc"], "2": ["chassis.dbc"] }
}
```

## Testing

Tests are hermetic — a small (250-byte) `sample.blf` generated with python-can
is committed under `test_data/` (regenerate with `test_data/gen_blf.py`). The
tests read it and assert the decoded frames and an end-to-end DBC decode.

```bash
ctest --test-dir <build> -R 'blf|can_dbc'
```

## Known Limitations

- **CAN FD is out of scope in v1** — lblf has no CAN-FD parse struct, and the DBC
  decoder rejects payloads larger than 8 bytes. CAN-FD / LIN / other objects are
  counted as skipped.
- Multiplexed CAN signals are not decoded (a `dbc_parser_cpp` limitation).
- The dialog exposes channels 1-4; additional channels are configured via the
  saved `channel_dbcs` map.
