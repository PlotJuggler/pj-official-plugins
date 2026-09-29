# JSON Message Parser

Decodes JSON (and CBOR, MessagePack, BSON) messages into typed fields.

## Supported Types

- numbers — always `double`, integers included: JSON writes `1.0` as `1`, so a
  field's integer/float split is not stable across messages, while a column
  keeps the type of its first value. PlotJuggler stores whole-number columns
  compactly and still shows them in the State Transitions view. Integers above
  2^53 lose precision.
- `bool` — boolean values
- strings shorter than 100 characters
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
- `use_embedded_timestamp` (bool, default `false`): use a top-level numeric
  field from the JSON message as the record timestamp (in seconds) instead of
  the host-provided receive time.
- `timestamp_field_name` (string, default `"timestamp"`): name of that
  top-level field. Configurable from the options dialog.

## Encoding

Registered as parser for `"json"` encoding.

## Known Limitations

- Only JSON format supported (original also handled CBOR, BSON, MessagePack)
