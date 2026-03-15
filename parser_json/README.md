# JSON Message Parser

Decodes JSON messages into typed fields, preserving native integer
precision (int64, uint64) instead of coercing everything to double.

## Supported Types

- `int64_t`, `uint64_t` — preserved as native integers
- `double` — floating-point values
- `bool` — boolean values
- Nested objects flattened with `/` separator (e.g. `pose/position/x`)
- Arrays flattened with bracket notation (e.g. `joints[0]`, `joints[1]`)

## Encoding

Registered as parser for `"json"` encoding.

## Known Limitations

- Only JSON format supported (original also handled CBOR, BSON, MessagePack)
- No configuration dialog for timestamp field name selection
