# Protocol Buffers Message Parser

Decodes Protocol Buffers messages using dynamic reflection from
FileDescriptorSet schemas.

## Supported Types

- All scalar types: `float`, `double`, `int32`, `int64`, `uint32`, `uint64`, `bool`
- Nested messages (recursive flattening with `/` separator)
- Repeated fields with bracket notation (`field[0]`, `field[1]`)
- Map fields with key-based paths
- String fields included if shorter than 100 bytes
- Enum values stored as human-readable name strings (e.g. `"OK"`, `"ERROR"`)
- Embedded timestamp auto-detection: if the message has a `double` field named
  `timestamp`, it is used as the series timestamp (opt-in via config JSON)
- Array size clamping: oversized repeated fields are skipped or truncated
  based on `max_array_size` and `clamp_large_arrays` config options

## Encoding

Registered as parser for `"protobuf"` encoding.

## Schema

Expects a `FileDescriptorSet` binary blob passed via `bindSchema()`.
The parser builds a `DescriptorPool` and uses `DynamicMessageFactory`
for reflection-based decoding. Transitive imports must be included in
the `FileDescriptorSet` (the MQTT source compiles them automatically via
`DiskSourceTree` + `Importer`).

## Configuration (JSON)

```json
{
  "use_embedded_timestamp": false,
  "max_array_size": 1000,
  "clamp_large_arrays": true
}
```

| Key | Default | Description |
|-----|---------|-------------|
| `use_embedded_timestamp` | `false` | Use `timestamp` double field as series timestamp |
| `max_array_size` | `1000` | Maximum number of repeated field elements to parse |
| `clamp_large_arrays` | `true` | Skip elements beyond `max_array_size` instead of failing |

## Bug fix vs original PlotJuggler

The original `ParserProtobuf` used `std::max` instead of `std::min` when
clamping large repeated fields, so the clamp never actually truncated the
array. This port uses direct assignment (`count = max_array_size_`), which
correctly limits the element count.
