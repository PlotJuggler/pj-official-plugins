# ULog Data Loader

Imports [PX4 ULog](https://docs.px4.io/main/en/dev_log/ulog_file_format.html)
flight log files.

## Features

- Full ULog binary format parsing via the `ulog_cpp` library
- Multi-instance message support (appends `.00`, `.01` suffixes)
- Array field flattening with index suffixes
- Padding field skipping (`_padding*` fields ignored)
- Parameters extracted and written as `_parameters/` topic

## Timestamp Handling

Timestamps are extracted from the first 8 bytes of each data message
(standard ULog format: `uint64_t timestamp` in microseconds, converted
to nanoseconds).

## Known Limitations

- Parameters/info/log messages dialog not yet available (requires SDK
  plugin status panel design)
- Timestamp field assumed at fixed offset rather than detected from format
