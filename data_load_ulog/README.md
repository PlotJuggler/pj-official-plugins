# ULog Data Loader

Imports [PX4 ULog](https://docs.px4.io/main/en/dev_log/ulog_file_format.html) flight logs, parsed with `ulog_cpp`.
Requires PlotJuggler 4.2 or later.

## Features

- Multi-instance messages get `.00`, `.01` suffixes; array fields are flattened with index suffixes.
- `char[N]` fields are one **string** series (not N numeric series); the value stops at the first NUL, or spans the
  whole array when there is none.
- `_padding*` fields are skipped. A record is rejected as corrupt only when it is shorter than the format's *logged*
  size, which excludes a trailing `_padding*` field: per the ULog spec that field may be omitted from the data. Padding
  anywhere else (including inside a nested message) still counts toward the required size.
- Parameters go to one `_parameters` topic with a field per parameter: the initial values at the start of the log (the
  earliest of the header timestamp, the first record and the first logged message), plus every in-flight change as a
  further point.
- File info goes to one `_info` topic with a field per key (numeric or text, e.g. `sys_name`, `ver_hw`), at the start of
  the log.
- Embedded log messages become `kLog` objects on the object topic `_log`, for the Log panel: severity `0`..`7` maps to
  fatal (0-2), error, warning, info (5-6), debug; a PX4 module prefix (`[commander] ...`) becomes the source, and the
  tag of a tagged message the category.
- No load dialog. The INFO messages and the last value of each parameter are also published as dataset metadata
  (`info`, `parameters`), shown in the host's dataset info view.
- Truncated or partially corrupt files import everything decodable; recoverable parse problems are reported as a
  warning.

## Timestamp Handling

Each message's `uint64_t timestamp` field (microseconds) is located **by
name** in the message format, as the ULog spec requires the field but not its
position. A message format with no timestamp field falls back to the sample
index as its time, matching the original PlotJuggler plugin, and a warning is
emitted.

Data-section parameter changes carry no timestamp of their own; each is stamped
with the timestamp of the most recent data message before it (or the file
start when none has been seen yet).

## Benchmark

`ulog_import_bench` imports a `.ulg` file through the built plugin into a host that discards every record, so it
measures the plugin's own cost (file read, `ulog_cpp` parse, decode, host calls) without the host datastore.

```bash
./build.sh data_load_ulog
build/data_load_ulog/Release/data_load_ulog/ulog_import_bench /path/to/flight.ulg 3
```

```
ulog_import_bench [--values] [--plugin <path.so>] [file.ulg] [iterations]
```

- `file.ulg`: the log to import. The default is the committed 1 MB sample, good only as a smoke run.
- `iterations`: default 3. The best run is summarised.
- `--plugin`: run another build of the plugin (e.g. one built from `main`) through the same host, to compare builds.
- `--values`: fold every decoded value into the checksum. This costs time in the host: use it to check two builds
  produce the same data, leave it off when timing.

Per run it prints the time of `loadConfig`, of the first pass (`parse_s`, which only collects the subscriptions) and of
the second pass (`decode_s`, which reads, decodes and writes the records), then throughput and peak RSS for the best
run, and the topic, field, record and value counts with an order-independent checksum.
