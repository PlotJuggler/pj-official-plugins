# ROS Message Parser

Decodes ROS 1 and ROS 2 (CDR) messages using the `rosx_introspection`
library, with special handling for common message types.

## Supported Types

- All ROS primitive types with native precision (int8 through uint64)
- ROS Time/Duration converted to double
- Nested messages flattened with `/` separator
- Arrays with bracket notation, max 500 elements by default

## Encodings

Registered for `"ros1msg"`, `"ros2msg"`, and `"cdr"` encodings.

## Special Message Handlers

- **Quaternion** — auto-detected in schemas, adds roll/pitch/yaw fields
- **JointState** — per-joint position/velocity/effort series
- **DiagnosticArray** — key-value pair extraction
- **TF2** — frame hierarchy with position/rotation
- **DataTamer** — schema/snapshot cross-message state
- **PAL Statistics** — name/value matching across messages
- **TSL** — definition/value buffering

## Timestamp Handling

Extracts `header.stamp` (sec + nanosec) when `use_embedded_timestamp`
is enabled.

## Known Limitations

- Array policy hardcoded to KEEP (original allowed DISCARD for large arrays)
- Truncation check for int64-to-double precision loss removed
- No interactive configuration dialog
