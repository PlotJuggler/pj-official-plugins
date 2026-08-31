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
| `flatten_structs` | bool | `true` | Flatten nested structs depth-first to slash-separated leaf names. `false` preserves struct boundaries while unsupported top-level struct columns are removed. |
| `synthetic_interval_ns` | int64 | `0` | When an axis is synthesized, add this many nanoseconds per row. Zero gives every row the message timestamp; negative intervals are allowed. The row index continues across record batches. |
| `max_array_size` | uint32 | `500` | Maximum scalar columns emitted for one list. Zero means unlimited. |
| `array_policy` | string | `"clamp"` | When a list width exceeds `max_array_size`, `"clamp"` keeps the first elements and `"skip"` drops the whole list column. |

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
`uint64`, `float`, `double`, `bool`, and `utf8`. Binary, decimal,
date/time/duration, maps/unions, complex lists, and unflattened struct columns
are removed from the shaped stream and reported. When parser runtime
diagnostics are available, the parser reports each unchanged dropped set once
as `parser_arrow.dropped_columns`. A schema with no host-ingestible data column
other than its timestamp axis is rejected. The selected timestamp axis is never
droppable.

## List expansion

`list<P>`, `large_list<P>`, and `fixed_size_list<P>` columns expand into scalar
columns named `name[0]`, `name[1]`, and so on. Expansion works at every struct
flattening depth, for example `pose/vel[2]`. `P` may be any primitive supported
by the host. Timestamp elements are converted to int64 nanoseconds, and
`large_string` or `string_view` elements are normalized to `utf8` by the same
rules as scalar columns.

The output width is fixed before the shaped schema is exposed:

- A `fixed_size_list<P, N>` uses its schema width `N`.
- A variable `list` or `large_list` uses the maximum list length in the first
  record batch of that message. The parser peeks and buffers that batch, then
  returns it as the wrapper stream's first batch. Later wider rows are limited
  to the already-selected width.
- An all-null/empty first batch selects width zero. The list emits no scalar
  columns and is reported in `dropped_columns` with an empty marker such as
  `+l(empty)`, even if a later batch contains values.

Null list rows produce nulls in every expanded column. Short rows are padded
with nulls; extra elements are ignored. A null struct ancestor also makes every
expanded descendant null.

`max_array_size` and `array_policy` use PlotJuggler's cross-parser `ArrayLimit`
contract. The default keeps at most the first 500 elements. `max_array_size: 0`
is unlimited. With `"clamp"`, an oversized width is reduced to the configured
limit; with `"skip"`, the source list is removed and reported with its original
Arrow format.

Lists of non-ingestible elements—including structs, nested lists, binary, and
dictionary values—remain unsupported and follow the normal dropped-column
diagnostic path at the shaping layer. Dictionary-encoded IPC fields are still
rejected earlier by `nanoarrow_ipc` as described below.

Dictionary-encoded fields are rejected at decode time: `nanoarrow_ipc` 0.7
refuses them while converting the IPC schema (so the offending field cannot be
named) and cannot decode `DictionaryBatch` messages. Producers must decode
dictionaries before encoding the IPC stream.

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
- Decode dictionary-encoded columns before IPC encoding.
- Do not use LZ4 compression.
- Use flat columns, nested structs, or primitive-element lists; pre-cast
  unsupported list elements and account for the first-batch width rule.
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
