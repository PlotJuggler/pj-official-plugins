# JSON Message Parser

Decodes JSON messages into typed fields, preserving native integer
precision (int64, uint64) instead of coercing everything to double.

## Supported Types

- `int64_t`, `uint64_t` — preserved as native integers
- `double` — floating-point values
- `bool` — boolean values
- Nested objects flattened with `/` separator (e.g. `pose/position/x`)
- Arrays flattened with bracket notation (e.g. `joints[0]`, `joints[1]`)

## Configuration

- `label_keyed_arrays` (bool, default `false`): arrays of objects carrying a
  string `"label"` (or `"name"`) member are keyed by that label instead of the
  index; when the only other member is `"value"`, it collapses to
  `arr/<label>` = value (the LeRobot/JointState name-value convention).
  Elements without a label, or repeating a label within the same array, keep
  the indexed name. Off by default so existing saved layouts keep their series
  names; the Foxglove bridge enables it for its json channels.

## Encoding

Registered as parser for `"json"` encoding.

## Known Limitations

- Only JSON format supported (original also handled CBOR, BSON, MessagePack)
- No configuration dialog for timestamp field name selection
