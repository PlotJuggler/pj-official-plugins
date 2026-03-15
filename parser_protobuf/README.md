# Protocol Buffers Message Parser

Decodes Protocol Buffers messages using dynamic reflection from
FileDescriptorSet schemas.

## Supported Types

- All scalar types: float, double, int32, int64, uint32, uint64, bool
- Nested messages (recursive flattening with `/` separator)
- Repeated fields with bracket notation (`field[0]`, `field[1]`)
- Map fields with key-based paths
- Pre-registration of fields at schema bind time for performance

## Encoding

Registered as parser for `"protobuf"` encoding.

## Schema

Expects a `FileDescriptorSet` binary blob passed via `bindSchema()`.
The parser builds a `DescriptorPool` and uses `DynamicMessageFactory`
for reflection-based decoding.

## Known Limitations

- Embedded timestamp extraction not implemented (original auto-detects
  "timestamp" double field)
- Enum values stored as int32 (original stored as human-readable strings)
- String fields skipped entirely (original included strings < 100 bytes)
- No array size clamping/discard policy
- No interactive dialog for .proto file loading
