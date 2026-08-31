# Arrow IPC Message Parser

`parser_arrow` is a PlotJuggler `MessageParser` for the `"arrow-ipc"`
encoding. Each message payload must be one complete Arrow IPC **stream**: a
schema message, its record batches, and the end-of-stream marker. Arrow IPC
**file** format payloads are not accepted. The payload is self-describing, so
the binding's `schema` bytes are informational and ignored.

The parser uses `nanoarrow` and `nanoarrow_ipc`, not `libarrow`.

## Configuration

Configuration is a JSON object:

| Key | Type | Default | Semantics |
|-----|------|---------|-----------|
| `timestamp_column` | string | `""` | Non-empty: use this exact top-level field as the time axis; an absent field is an error. Empty: auto-detect an axis, then synthesize one if none is found. |
| `flatten_structs` | bool | `true` | Flatten nested structs depth-first to slash-separated leaf names. `false` preserves top-level struct columns, which the current host skips. |
| `synthetic_interval_ns` | int64 | `0` | When an axis is synthesized, add this many nanoseconds per row. Zero gives every row the message timestamp; negative intervals are allowed. The row index continues across record batches. |

The top-level JSON value must be an object; unknown keys are ignored.

## Timestamp axis

Axis selection is ordered:

1. A non-empty `timestamp_column`. The named top-level field must exist.
2. Automatic detection: the first top-level Arrow `TIMESTAMP` field in schema
   order, then the first field matching this name priority:
   `timestamp_ns`, `recording_timestamp_ns`, `timestamp`, `time`, `ts`.
3. Synthesis of an int64 `timestamp_ns` field as
   `message_timestamp_ns + row_index * synthetic_interval_ns`. The row index
   is stream-wide, not batch-local.

The selected axis is emitted as int64 nanoseconds. Accepted source types are:

- `int32`, `int64`, `uint32`, and `uint64`: treated as nanoseconds, widened to
  int64 without scaling.
- `float` and `double`: treated as seconds, multiplied by 1e9, and rounded to
  the nearest nanosecond (halfway values away from zero).
- Arrow `TIMESTAMP`: scaled from its second, millisecond, microsecond, or
  nanosecond unit. Timezone metadata is ignored.

A null axis value, a non-finite floating value, or any conversion or synthesis
overflow is an error; values are never wrapped. Synthesis exists because the
host interprets an empty timestamp-column name as a request to use row indices,
which would discard the transport message time.

## Column shaping and host support

- With `flatten_structs: true`, struct leaves use PlotJuggler 3's
  `parent/child` naming convention. Nullable and sliced struct parents are
  applied to every flattened leaf.
- Every Arrow `TIMESTAMP` column, not only the selected axis, is cast to int64
  nanoseconds.
- `large_string`, `large_binary`, `string_view`, and `binary_view` are
  normalized to `utf8` or `binary`. However, `nanoarrow_ipc` 0.7 cannot decode
  IPC fields of type `string_view` or `binary_view` at all; producers must cast
  them to `utf8` or `binary` before encoding the stream.
- Empty field names become `_<i>`, where `i` is the field's child index at that
  level. Duplicate output names after flattening and substitution are errors.

The current PlotJuggler host ingests `int8` through `int64`, `uint8` through
`uint64`, `float`, `double`, `bool`, and `utf8`. It therefore skips lists,
binary, dictionary, decimal, date/time/duration, maps/unions, and unflattened
struct columns. The parser retains those columns in the shaped stream and, when
parser runtime diagnostics are available, reports each unchanged dropped set
once as `parser_arrow.dropped_columns`. A schema with no host-ingestible data
column other than its timestamp axis is rejected.

## Compression

Zstandard-compressed IPC record-batch bodies are supported. LZ4-compressed
bodies are rejected with an error naming `lz4`; `nanoarrow_ipc` 0.7 has no LZ4
backend.

## Out of scope for v1

Canonical objects such as images, point clouds, poses, and frame transforms are
not produced: `classifySchema` returns `kNone`. ROS/CDR-native payloads should be
bound by the transport to `parser_ros`, not `parser_arrow`.

## Producer checklist

- Encode one complete Arrow IPC stream per message, not an IPC file.
- Prefer an explicit int64-nanosecond timestamp column and configure its name.
- Cast view types before IPC encoding.
- Do not use LZ4 compression.
- Use flat columns or nested struct columns only.
- Use one topic per parser binding.

## Build and test

```bash
./build.sh parser_arrow
ctest --test-dir build/parser_arrow/Release
```

Regenerate the checked-in fixtures with any Python environment containing
PyArrow 15 or newer:

```bash
python3 parser_arrow/test_data/gen_fixtures.py
```

## Known dependency quirk

The ConanCenter `nanoarrow/0.7.0` recipe ships `libflatccrt.a` but omits it
from the `nanoarrow_ipc` component. `parser_arrow/CMakeLists.txt` resolves the
archive with `find_library`.
