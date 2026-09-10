# ULog Data Loader

Imports [PX4 ULog](https://docs.px4.io/main/en/dev_log/ulog_file_format.html)
flight log files.

## Features

- Full ULog binary format parsing via the `ulog_cpp` library
- Multi-instance message support (appends `.00`, `.01` suffixes)
- Array field flattening with index suffixes
- `char[N]` fields imported as **string** series (one series per field, not
  N numeric series); the value stops at the first NUL or spans the whole
  array when there is none
- Padding field skipping (`_padding*` fields ignored)
- Parameters written as `_parameters/<name>` series: the initial snapshot at
  file start, plus every **in-flight parameter change** as a further point
- File info (`_info/`) and embedded log messages (`_log`) written as topics
- Parameters / info / log messages dialog (Info, Properties, Message Logs)
- Progress reporting with "Stop and keep" support
- Truncated or partially corrupt files import everything decodable; recoverable
  parse problems are surfaced as a warning

## Upgrade note: `char[N]` series names

Before this change a `char[N]` field produced N numeric series named
`<topic>/<field>.00` … `<field>.NN`. It now produces one string series named
`<topic>/<field>`. Saved layouts that reference the old per-byte names will not
find those curves after upgrading.

## Timestamp Handling

Each message's `uint64_t timestamp` field (microseconds) is located **by
name** in the message format, as the ULog spec requires the field but not its
position. A message format with no timestamp field falls back to the sample
index as its time, matching the original PlotJuggler plugin, and a warning is
emitted.

Data-section parameter changes carry no timestamp of their own; each is stamped
with the timestamp of the most recent data message before it (or the file
start when none has been seen yet).
