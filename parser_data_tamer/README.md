# DataTamer Message Parser

Decodes [DataTamer](https://github.com/facontidavide/data_tamer) binary
snapshots from data_tamer 1.x and 2.x recordings into typed scalar fields.

## Features

- Line schemas (versions 4 and 5) and YAML schemas (version 6), parsed via `BuildSchemaFromText()`
- Native boolean, integer and floating-point types preserved; `char` emitted as a `uint8` wire byte
- Nested structs, fixed arrays and dynamic vectors, with names such as `/pose/position/x` and `/joints[0]/position`
- Field names prefixed with `/`; timestamps supplied by the host's message receive/log time
- Malformed snapshots rejected with an error instead of emitting partial records

Schema texts from data_tamer releases before 0.7.0, which lack a `### version:` line,
are not supported. Enabled fields with opaque custom encodings cannot be decoded.

## Encoding

Registered as parser for `"data_tamer"` encoding.

## Status

Fully ported with no significant gaps.
