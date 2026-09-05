# Arrow IPC Message Parser

`parser_arrow` is a PlotJuggler `MessageParser` for the `"arrow-ipc"`
encoding. Each message payload must be one Arrow IPC **stream**: a schema
message followed by its record batches. The stream is read to its end-of-stream
marker or to the end of the payload, whichever comes first; data after the
end-of-stream marker is ignored. Arrow IPC **file** format payloads are not
accepted. The payload is self-describing, so the binding's `schema` bytes are
informational and ignored.

The parser uses `nanoarrow` and `nanoarrow_ipc`, not `libarrow`.

## Configuration

Configuration is a JSON object:

| Key | Type | Default | Semantics |
|-----|------|---------|-----------|
| `timestamp_column` | string | `""` | Non-empty: use this leaf as the time axis, named as a flattened path at any depth. Dots normalize to `/` as they do in leaf names, so `header.stamp` and `header/stamp` are the same request; a name matching no leaf is an error. Empty: auto-detect an axis, then synthesize one if none is found. |
| `timestamp_unit` | string | `"ns"` | Integer-axis unit: `"ns"`, `"us"`, `"ms"`, or `"s"`. Native Arrow timestamps retain their own unit; floating axes always carry seconds. Invalid units are rejected. |
| `flatten_structs` | bool | `true` | Flatten nested structs depth-first to slash-separated leaf names. `false` preserves struct boundaries while unsupported top-level struct columns are removed. |
| `synthetic_interval_ns` | int64 | `0` | When an axis is synthesized, add this many nanoseconds per row. Zero gives every row the message timestamp; negative intervals are allowed. The row index continues across record batches. |
| `max_array_size` | uint32 | `500` | Maximum scalar columns emitted for one list. Zero means unlimited. |
| `array_policy` | string | `"clamp"` | When a list width exceeds `max_array_size`, `"clamp"` keeps the first elements and `"skip"` drops the whole list column. |

The top-level JSON value must be an object; unknown keys are ignored.

## Timestamp axis

Configuration and detection both walk the leaves described in
[Column shaping](#column-shaping-and-host-support), so an axis nested inside a
struct is reachable by its normalized name (with `flatten_structs: false` there
is no such leaf and only top-level fields remain). Selection is ordered:

1. A non-empty `timestamp_column`, matched against the full flattened leaf name
   after the same dot normalization, so a field literally named `sensor.time`
   answers to both `sensor.time` and `sensor/time`. A name matching no leaf is
   an error naming the column as the configuration spelled it.
2. Automatic detection: the first Arrow `TIMESTAMP`-typed leaf in schema order,
   then the first scalar leaf whose full flattened name matches this name
   priority: `timestamp_ns`, `recording_timestamp_ns`, `timestamp`, `time`,
   `ts`, `t`, `time_stamp`, `datetime`, `date_time`, `_timestamp`, `_time`.
   The SDK policy checks exact spelling before ASCII case-insensitive matches
   at each priority. At the default nanosecond unit, eligible storage is
   `TIMESTAMP`, `int64`, `uint64`, or `double`. At seconds, `int32` and `uint32`
   also qualify. Narrower integers and `float` require explicit selection.
3. Synthesis of an int64 `timestamp_ns` field as
   `message_timestamp_ns + row_index * synthetic_interval_ns`. The row index
   is stream-wide, not batch-local. Runtime diagnostics announce the synthetic
   axis as `parser_arrow.synthetic_timestamp` and name the configured interval.

Automatic candidates are scalar leaves only. An expanded list is never selected
by its element type or by its source list name, so `list<int64>` named
`timestamp` cannot displace a scalar `time` axis.

The selected axis is emitted as int64 nanoseconds. Accepted source types are:

- every integer width, `int8` through `int64` and `uint8` through `uint64`:
  scaled from `timestamp_unit` to int64 nanoseconds (default `ns`). A `uint64`
  tick above `INT64_MAX`, or overflow during scaling, is an error naming the column.
- `float` and `double`: treated as seconds, multiplied by 1e9, and rounded to
  the nearest nanosecond (halfway values away from zero).
- Arrow `TIMESTAMP`: scaled from its second, millisecond, microsecond, or
  nanosecond unit. Timezone metadata is ignored.

A null axis value, a non-finite floating value, or any conversion or synthesis
overflow is an error; values are never wrapped. Synthesis exists because the
host interprets an empty timestamp-column name as a request to use row indices,
which would discard the transport message time.

Explicit configuration deliberately remains more permissive than automatic
detection. Selecting integer storage too narrow for the configured unit, or
`float`, emits the SDK's range/precision warning as
`parser_arrow.narrow_timestamp_axis`. Int32/uint32 seconds do not need that warning. If an
explicit float32 axis reaches `|seconds| >= 2^23` while the stream is drained,
the parser also reports `parser_arrow.float_axis_precision`. A `double` remains
plausible, but its spacing at the present epoch is about 238 ns, so it cannot
represent every individual nanosecond there. `HALF_FLOAT` is not an accepted
axis type, even explicitly.

Plan-time and drain-time warnings use change-based deduplication: the same
ordered set of diagnostic codes and messages is emitted once until that set
changes.

## Column shaping and host support

- With `flatten_structs: true`, struct leaves use PlotJuggler 3's
  `parent/child` naming convention, flattened depth-first. Nullable and sliced
  struct parents are applied to every flattened leaf.
- A literal `.` inside any field-name component is a path separator too and is
  replaced by `/`. So `wheel.speed` becomes `wheel/speed`, and a field
  `header.stamp` inside a struct `msg` becomes `msg/header/stamp`. A flat dotted
  name and the equivalent nested struct therefore produce the same series name.
- Every Arrow `TIMESTAMP` column, not only the selected axis, is cast to int64
  nanoseconds.
- `large_string`, `large_binary`, `string_view`, and `binary_view` are
  normalized to `utf8` or `binary`. However, `nanoarrow_ipc` 0.7 cannot decode
  IPC fields of type `string_view` or `binary_view` at all; producers must cast
  them to `utf8` or `binary` before encoding the stream.
- Empty field names become `_<i>`, where `i` is the field's child index at that
  level. Duplicate output names after flattening, dot replacement, and empty-name
  substitution are errors.

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
  to the already-selected width. After draining, the parser reports their count
  and the first affected column as `parser_arrow.truncated_lists`.
- An all-null/empty first batch selects width zero. The list emits no scalar
  columns and is reported once at plan time with the explicit reason `empty in
  first batch`, even if a later batch contains values. Because the source list
  is absent from the output plan, later batches are not monitored for
  truncation. This is a known limitation. Its reason is kept separate from the
  `unsupported host type` wording used for genuinely unsupported columns.

Null list rows produce nulls in every expanded column. Short rows are padded
with nulls; extra elements are truncated and counted. A null struct ancestor
also makes every expanded descendant null.

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

Zstandard-compressed IPC record-batch bodies are supported. If any record-batch
header selects LZ4, the stream is rejected before ingest with an error naming
`lz4`; `nanoarrow_ipc` 0.7 has no LZ4 backend.

## Out of scope for v1

Canonical objects such as images, point clouds, poses, and frame transforms are
not produced: `classifySchema` returns `kNone`. ROS/CDR-native payloads should be
bound by the transport to `parser_ros`, not `parser_arrow`.

## Producer checklist

- Encode one Arrow IPC stream per message, not an IPC file.
- Prefer an explicit int64-nanosecond timestamp column and configure its name.
  Either spelling works for a nested or dotted field: `header.stamp` and
  `header/stamp` name the same leaf.
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
